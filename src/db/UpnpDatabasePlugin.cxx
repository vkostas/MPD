/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "UpnpDatabasePlugin.hxx"
#include "upnp/Domain.hxx"
#include "upnp/upnpplib.hxx"
#include "upnp/Discovery.hxx"
#include "upnp/ContentDirectoryService.hxx"
#include "upnp/Directory.hxx"
#include "upnp/Tags.hxx"
#include "upnp/Util.hxx"
#include "DatabasePlugin.hxx"
#include "DatabaseSelection.hxx"
#include "DatabaseError.hxx"
#include "LightDirectory.hxx"
#include "LightSong.hxx"
#include "ConfigData.hxx"
#include "tag/TagBuilder.hxx"
#include "tag/TagTable.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "fs/Traits.hxx"
#include "Log.hxx"
#include "SongFilter.hxx"

#include <string>
#include <vector>
#include <set>

#include <assert.h>
#include <string.h>

static const char *const rootid = "0";

class UpnpSong : public LightSong {
	std::string uri2, real_uri2;

	Tag tag2;

public:
	UpnpSong(UPnPDirObject &&object, std::string &&_uri)
		:uri2(std::move(_uri)),
		 real_uri2(std::move(object.url)),
		 tag2(std::move(object.tag)) {
		directory = nullptr;
		uri = uri2.c_str();
		real_uri = real_uri2.c_str();
		tag = &tag2;
		mtime = 0;
		start_ms = end_ms = 0;
	}
};

class UpnpDatabase : public Database {
	LibUPnP *m_lib;
	UPnPDeviceDirectory *m_superdir;

public:
	static Database *Create(EventLoop &loop, DatabaseListener &listener,
				const config_param &param,
				Error &error);

	virtual bool Open(Error &error) override;
	virtual void Close() override;
	virtual const LightSong *GetSong(const char *uri_utf8,
					 Error &error) const override;
	virtual void ReturnSong(const LightSong *song) const;

	virtual bool Visit(const DatabaseSelection &selection,
			   VisitDirectory visit_directory,
			   VisitSong visit_song,
			   VisitPlaylist visit_playlist,
			   Error &error) const override;

	virtual bool VisitUniqueTags(const DatabaseSelection &selection,
				     TagType tag_type,
				     VisitString visit_string,
				     Error &error) const override;

	virtual bool GetStats(const DatabaseSelection &selection,
			      DatabaseStats &stats,
			      Error &error) const override;
	virtual time_t GetUpdateStamp() const {return 0;}

protected:
	bool Configure(const config_param &param, Error &error);

private:
	bool VisitServer(ContentDirectoryService &server,
			 const std::list<std::string> &vpath,
			 const DatabaseSelection &selection,
			 VisitDirectory visit_directory,
			 VisitSong visit_song,
			 VisitPlaylist visit_playlist,
			 Error &error) const;

	/**
	 * Run an UPnP search according to MPD parameters, and
	 * visit_song the results.
	 */
	bool SearchSongs(ContentDirectoryService &server,
			 const char *objid,
			 const DatabaseSelection &selection,
			 VisitSong visit_song,
			 Error &error) const;

	bool SearchSongs(ContentDirectoryService &server,
			 const char *objid,
			 const DatabaseSelection &selection,
			 UPnPDirContent& dirbuf,
			 Error &error) const;

	bool Namei(ContentDirectoryService &server,
		   const std::list<std::string> &vpath,
		   UPnPDirObject &dirent,
		   Error &error) const;

	/**
	 * Take server and objid, return metadata.
	 */
	bool ReadNode(ContentDirectoryService &server,
		      const char *objid, UPnPDirObject& dirent,
		      Error &error) const;

	/**
	 * Get the path for an object Id. This works much like pwd,
	 * except easier cause our inodes have a parent id. Not used
	 * any more actually (see comments in SearchSongs).
	 */
	bool BuildPath(ContentDirectoryService &server,
		       const UPnPDirObject& dirent, std::string &idpath,
		       Error &error) const;
};

Database *
UpnpDatabase::Create(gcc_unused EventLoop &loop,
		     gcc_unused DatabaseListener &listener,
		     const config_param &param, Error &error)
{
	UpnpDatabase *db = new UpnpDatabase();
	if (!db->Configure(param, error)) {
		delete db;
		return nullptr;
	}

	/* libupnp loses its ability to receive multicast messages
	   apparently due to daemonization; using the LazyDatabase
	   wrapper works around this problem */
	return db;
}

bool
UpnpDatabase::Configure(const config_param &, Error &)
{
	return true;
}

bool
UpnpDatabase::Open(Error &error)
{
	m_lib = new LibUPnP();
	if (!m_lib->ok()) {
		error.Set(m_lib->GetInitError());
		delete m_lib;
		return false;
	}

	m_superdir = new UPnPDeviceDirectory(m_lib);
	if (!m_superdir->Start(error)) {
		delete m_superdir;
		delete m_lib;
		return false;
	}

	// Wait for device answers. This should be consistent with the value set
	// in the lib (currently 2)
	sleep(2);
	return true;
}

void
UpnpDatabase::Close()
{
	delete m_superdir;
	delete m_lib;
}

void
UpnpDatabase::ReturnSong(const LightSong *_song) const
{
	assert(_song != nullptr);

	UpnpSong *song = (UpnpSong *)const_cast<LightSong *>(_song);
	delete song;
}

// Get song info by path. We can receive either the id path, or the titles
// one
const LightSong *
UpnpDatabase::GetSong(const char *uri, Error &error) const
{
	auto vpath = stringToTokens(uri, "/", true);
	if (vpath.size() < 2) {
		error.Format(db_domain, DB_NOT_FOUND, "No such song: %s", uri);
		return nullptr;
	}

	ContentDirectoryService server;
	if (!m_superdir->getServer(vpath.front().c_str(), server, error))
		return nullptr;

	vpath.pop_front();

	UPnPDirObject dirent;
	if (vpath.front() != rootid) {
		if (!Namei(server, vpath, dirent, error))
			return nullptr;
	} else {
		if (!ReadNode(server, vpath.back().c_str(), dirent,
			      error))
			return nullptr;
	}

	return new UpnpSong(std::move(dirent), uri);
}

/**
 * Double-quote a string, adding internal backslash escaping.
 */
static void
dquote(std::string &out, const char *in)
{
	out.push_back('"');

	for (; *in != 0; ++in) {
		switch(*in) {
		case '\\':
		case '"':
			out.push_back('\\');
			out.push_back(*in);
			break;

		default:
			out.push_back(*in);
		}
	}

	out.push_back('"');
}

// Run an UPnP search, according to MPD parameters. Return results as
// UPnP items
bool
UpnpDatabase::SearchSongs(ContentDirectoryService &server,
			  const char *objid,
			  const DatabaseSelection &selection,
			  UPnPDirContent &dirbuf,
			  Error &error) const
{
	const SongFilter *filter = selection.filter;
	if (selection.filter == nullptr)
		return true;

	std::list<std::string> searchcaps;
	if (!server.getSearchCapabilities(m_lib->getclh(), searchcaps, error))
		return false;

	if (searchcaps.empty())
		return true;

	std::string cond;
	for (const auto &item : filter->GetItems()) {
		switch (auto tag = item.GetTag()) {
		case LOCATE_TAG_ANY_TYPE:
			{
				if (!cond.empty()) {
					cond += " and ";
				}
				cond += '(';
				bool first(true);
				for (const auto& cap : searchcaps) {
					if (first)
						first = false;
					else
						cond += " or ";
					cond += cap;
					if (item.GetFoldCase()) {
						cond += " contains ";
					} else {
						cond += " = ";
					}
					dquote(cond, item.GetValue().c_str());
				}
				cond += ')';
			}
			break;

		default:
			/* Unhandled conditions like
			   LOCATE_TAG_BASE_TYPE or
			   LOCATE_TAG_FILE_TYPE won't have a
			   corresponding upnp prop, so they will be
			   skipped */
			if (tag == TAG_ALBUM_ARTIST)
				tag = TAG_ARTIST;

			// TODO: support LOCATE_TAG_ANY_TYPE etc.
			const char *name = tag_table_lookup(upnp_tags,
							    TagType(tag));
			if (name == nullptr)
				continue;

			if (!cond.empty()) {
				cond += " and ";
			}
			cond += name;

			/* FoldCase doubles up as contains/equal
			   switch. UpNP search is supposed to be
			   case-insensitive, but at least some servers
			   have the same convention as mpd (e.g.:
			   minidlna) */
			if (item.GetFoldCase()) {
				cond += " contains ";
			} else {
				cond += " = ";
			}
			dquote(cond, item.GetValue().c_str());
		}
	}

	return server.search(m_lib->getclh(),
			     objid, cond.c_str(), dirbuf,
			     error);
}

static bool
visitSong(UPnPDirObject &&meta, std::string &&path,
	  const DatabaseSelection &selection,
	  VisitSong visit_song, Error& error)
{
	if (!visit_song)
		return true;

	const UpnpSong song(std::move(meta), std::move(path));
	return !selection.Match(song) || visit_song(song, error);
}

/**
 * Build synthetic path based on object id for search results. The use
 * of "rootid" is arbitrary, any name that is not likely to be a top
 * directory name would fit.
 */
static std::string
songPath(const std::string &servername,
	 const std::string &objid)
{
	return servername + "/" + rootid + "/" + objid;
}

bool
UpnpDatabase::SearchSongs(ContentDirectoryService &server,
			  const char *objid,
			  const DatabaseSelection &selection,
			  VisitSong visit_song,
			  Error &error) const
{
	UPnPDirContent dirbuf;
	if (!visit_song)
		return true;
	if (!SearchSongs(server, objid, selection, dirbuf, error))
		return false;

	for (auto &dirent : dirbuf.objects) {
		if (dirent.type != UPnPDirObject::Type::ITEM ||
		    dirent.item_class != UPnPDirObject::ItemClass::MUSIC)
			continue;

		// We get song ids as the result of the UPnP search. But our
		// client expects paths (e.g. we get 1$4$3788 from minidlna,
		// but we need to translate to /Music/All_Music/Satisfaction).
		// We can do this in two ways:
		//  - Rebuild a normal path using BuildPath() which is a kind of pwd
		//  - Build a bogus path based on the song id.
		// The first method is nice because the returned paths are pretty, but
		// it has two big problems:
		//  - The song paths are ambiguous: e.g. minidlna returns all search
		//    results as being from the "All Music" directory, which can
		//    contain several songs with the same title (but different objids)
		//  - The performance of BuildPath() is atrocious on very big
		//    directories, even causing timeouts in clients. And of
		//    course, 'All Music' is very big.
		// So we return synthetic and ugly paths based on the object id,
		// which we later have to detect.
		if (!visitSong(std::move(dirent),
			       songPath(server.getFriendlyName(),
					dirent.m_id),
			       selection, visit_song,
			       error))
			return false;
	}

	return true;
}

bool
UpnpDatabase::ReadNode(ContentDirectoryService &server,
		       const char *objid, UPnPDirObject &dirent,
		       Error &error) const
{
	UPnPDirContent dirbuf;
	if (!server.getMetadata(m_lib->getclh(), objid, dirbuf, error))
		return false;

	if (dirbuf.objects.size() == 1) {
		dirent = std::move(dirbuf.objects.front());
	} else {
		error.Format(upnp_domain, "Bad resource");
		return false;
	}

	return true;
}

bool
UpnpDatabase::BuildPath(ContentDirectoryService &server,
			const UPnPDirObject& idirent,
			std::string &path,
			Error &error) const
{
	const char *pid = idirent.m_id.c_str();
	path.clear();
	UPnPDirObject dirent;
	while (strcmp(pid, rootid) != 0) {
		if (!ReadNode(server, pid, dirent, error))
			return false;
		pid = dirent.m_pid.c_str();

		if (path.empty())
			path = dirent.name;
		else
			path = PathTraitsUTF8::Build(dirent.name.c_str(),
						     path.c_str());
	}

	path = PathTraitsUTF8::Build(server.getFriendlyName(),
				     path.c_str());
	return true;
}

// Take server and internal title pathname and return objid and metadata.
bool
UpnpDatabase::Namei(ContentDirectoryService &server,
		    const std::list<std::string> &vpath,
		    UPnPDirObject &odirent,
		    Error &error) const
{
	if (vpath.empty()) {
		// looking for root info
		if (!ReadNode(server, rootid, odirent, error))
			return false;

		return true;
	}

	const UpnpClient_Handle handle = m_lib->getclh();

	std::string objid(rootid);

	// Walk the path elements, read each directory and try to find the next one
	for (auto i = vpath.begin(), last = std::prev(vpath.end());; ++i) {
		UPnPDirContent dirbuf;
		if (!server.readDir(handle, objid.c_str(), dirbuf, error))
			return false;

		// Look for the name in the sub-container list
		UPnPDirObject *child = dirbuf.FindObject(i->c_str());
		if (child == nullptr) {
			error.Format(db_domain, DB_NOT_FOUND,
				     "No such object");
			return false;
		}

		if (i == last) {
			odirent = std::move(*child);
			return true;
		}

		if (child->type != UPnPDirObject::Type::CONTAINER) {
			error.Format(db_domain, DB_NOT_FOUND,
				     "Not a container");
			return false;
		}

		objid = std::move(child->m_id);
	}
}

// vpath is a parsed and writeable version of selection.uri. There is
// really just one path parameter.
bool
UpnpDatabase::VisitServer(ContentDirectoryService &server,
			  const std::list<std::string> &vpath,
			  const DatabaseSelection &selection,
			  VisitDirectory visit_directory,
			  VisitSong visit_song,
			  VisitPlaylist visit_playlist,
			  Error &error) const
{
	/* If the path begins with rootid, we know that this is a
	   song, not a directory (because that's how we set things
	   up). Just visit it. Note that the choice of rootid is
	   arbitrary, any value not likely to be the name of a top
	   directory would be ok. */
	/* !Note: this *can't* be handled by Namei further down,
	   because the path is not valid for traversal. Besides, it's
	   just faster to access the target node directly */
	if (!vpath.empty() && vpath.front() == rootid) {
		if (visit_song) {
			UPnPDirObject dirent;
			if (!ReadNode(server, vpath.back().c_str(), dirent,
				      error))
				return false;

			std::string path = songPath(server.getFriendlyName(),
						    dirent.m_id);
			if (!visitSong(std::move(dirent), path.c_str(),
				       selection,
				       visit_song, error))
				return false;
		}
		return true;
	}

	// Translate the target path into an object id and the associated metadata.
	UPnPDirObject tdirent;
	if (!Namei(server, vpath, tdirent, error))
		return false;

	/* If recursive is set, this is a search... No use sending it
	   if the filter is empty. In this case, we implement limited
	   recursion (1-deep) here, which will handle the "add dir"
	   case. */
	if (selection.recursive && selection.filter)
		return SearchSongs(server, tdirent.m_id.c_str(), selection,
				   visit_song, error);

	if (tdirent.type == UPnPDirObject::Type::ITEM) {
		// Target is a song. Not too sure we ever get there actually, maybe
		// this is always catched by the special uri test above.
		switch (tdirent.item_class) {
		case UPnPDirObject::ItemClass::MUSIC:
			if (visit_song)
				return visitSong(std::move(tdirent),
						 std::string(selection.uri),
						 selection, visit_song,
						 error);
			break;

		case UPnPDirObject::ItemClass::PLAYLIST:
			if (visit_playlist) {
				/* Note: I've yet to see a playlist
				 item (playlists seem to be usually
				 handled as containers, so I'll decide
				 what to do when I see one... */
			}
			break;

		case UPnPDirObject::ItemClass::UNKNOWN:
			break;
		}

		return true;
	}

	/* Target was a a container. Visit it. We could read slices
	   and loop here, but it's not useful as mpd will only return
	   data to the client when we're done anyway. */
	UPnPDirContent dirbuf;
	if (!server.readDir(m_lib->getclh(), tdirent.m_id.c_str(), dirbuf,
			    error))
		return false;

	for (auto &dirent : dirbuf.objects) {
		switch (dirent.type) {
		case UPnPDirObject::Type::UNKNOWN:
			assert(false);
			gcc_unreachable();

		case UPnPDirObject::Type::CONTAINER:
			if (visit_directory) {
				const std::string uri = PathTraitsUTF8::Build(selection.uri.c_str(),
									      dirent.name.c_str());
				const LightDirectory d(uri.c_str(), 0);
				if (!visit_directory(d, error))
					return false;
			}

			break;

		case UPnPDirObject::Type::ITEM:
			switch (dirent.item_class) {
			case UPnPDirObject::ItemClass::MUSIC:
				if (visit_song) {
					/* We identify songs by giving
					   them a special path. The Id
					   is enough to fetch them
					   from the server anyway. */

					std::string p;
					if (!selection.recursive)
						p = PathTraitsUTF8::Build(selection.uri.c_str(),
									  dirent.name.c_str());

					if (!visitSong(std::move(dirent),
						       p.c_str(),
						       selection, visit_song, error))
						return false;
				}

				break;

			case UPnPDirObject::ItemClass::PLAYLIST:
				if (visit_playlist) {
					/* Note: I've yet to see a
					   playlist item (playlists
					   seem to be usually handled
					   as containers, so I'll
					   decide what to do when I
					   see one... */
				}

				break;

			case UPnPDirObject::ItemClass::UNKNOWN:
				break;
			}
		}
	}

	return true;
}

// Deal with the possibly multiple servers, call VisitServer if needed.
bool
UpnpDatabase::Visit(const DatabaseSelection &selection,
		    VisitDirectory visit_directory,
		    VisitSong visit_song,
		    VisitPlaylist visit_playlist,
		    Error &error) const
{
	auto vpath = stringToTokens(selection.uri, "/", true);
	if (vpath.empty()) {
		std::vector<ContentDirectoryService> servers;
		if (!m_superdir->getDirServices(servers, error))
			return false;

		if (!selection.recursive) {
			// If the path is empty and recursive is not set, synthetize a
			// pseudo-directory from the list of servers.
			if (visit_directory) {
				for (auto& server : servers) {
					const LightDirectory d(server.getFriendlyName(), 0);
					if (!visit_directory(d, error))
						return false;
				}
			}
		} else {
			// Recursive is set: visit each server
			for (auto& server : servers) {
				if (!VisitServer(server, vpath, selection,
						 visit_directory, visit_song, visit_playlist, error))
					return false;
			}
		}
		return true;
	}

	// We do have a path: the first element selects the server
	std::string servername(std::move(vpath.front()));
	vpath.pop_front();

	ContentDirectoryService server;
	if (!m_superdir->getServer(servername.c_str(), server, error))
		return false;

	return VisitServer(server, vpath, selection,
			   visit_directory, visit_song, visit_playlist, error);
}

bool
UpnpDatabase::VisitUniqueTags(const DatabaseSelection &selection,
			      TagType tag,
			      VisitString visit_string,
			      Error &error) const
{
	if (!visit_string)
		return true;

	std::vector<ContentDirectoryService> servers;
	if (!m_superdir->getDirServices(servers, error))
		return false;

	std::set<std::string> values;
	for (auto& server : servers) {
		UPnPDirContent dirbuf;
		if (!SearchSongs(server, rootid, selection, dirbuf, error))
			return false;

		for (const auto &dirent : dirbuf.objects) {
			if (dirent.type != UPnPDirObject::Type::ITEM ||
			    dirent.item_class != UPnPDirObject::ItemClass::MUSIC)
				continue;

			const char *value = dirent.tag.GetValue(tag);
			if (value != nullptr) {
#if defined(__clang__) || GCC_CHECK_VERSION(4,8)
				values.emplace(value);
#else
				values.insert(value);
#endif
			}
		}
	}

	for (const auto& value : values)
		if (!visit_string(value.c_str(), error))
			return false;

	return true;
}

bool
UpnpDatabase::GetStats(const DatabaseSelection &,
		       DatabaseStats &stats, Error &) const
{
	/* Note: this gets called before the daemonizing so we can't
	   reallyopen this would be a problem if we had real stats */
	stats.song_count = 0;
	stats.total_duration = 0;
	stats.artist_count = 0;
	stats.album_count = 0;
	return true;
}

const DatabasePlugin upnp_db_plugin = {
	"upnp",
	UpnpDatabase::Create,
};
