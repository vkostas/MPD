/*
 * Copyright 2003-2017 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "DatabaseCommands.hxx"
#include "Request.hxx"
#include "db/DatabaseQueue.hxx"
#include "db/DatabasePlaylist.hxx"
#include "db/DatabasePrint.hxx"
#include "db/Count.hxx"
#include "db/Selection.hxx"
#include "CommandError.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "tag/ParseName.hxx"
#include "tag/Mask.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Exception.hxx"
#include "util/StringAPI.hxx"
#include "util/ASCII.hxx"
#include "song/Filter.hxx"
#include "BulkEdit.hxx"

#include <memory>

CommandResult
handle_listfiles_db(Client &client, Response &r, const char *uri)
{
	const DatabaseSelection selection(uri, false);
	db_selection_print(r, client.GetPartition(),
			   selection, false, true);
	return CommandResult::OK;
}

CommandResult
handle_lsinfo2(Client &client, const char *uri, Response &r)
{
	const DatabaseSelection selection(uri, false);
	db_selection_print(r, client.GetPartition(),
			   selection, true, false);
	return CommandResult::OK;
}

static TagType
ParseSortTag(const char *s)
{
	if (StringIsEqualIgnoreCase(s, "Last-Modified"))
		return TagType(SORT_TAG_LAST_MODIFIED);

	TagType tag = tag_name_parse_i(s);
	if (tag == TAG_NUM_OF_ITEM_TYPES)
		throw ProtocolError(ACK_ERROR_ARG, "Unknown sort tag");

	return tag;
}

static CommandResult
handle_match(Client &client, Request args, Response &r, bool fold_case)
{
	RangeArg window;
	if (args.size >= 2 && StringIsEqual(args[args.size - 2], "window")) {
		window = args.ParseRange(args.size - 1);

		args.pop_back();
		args.pop_back();
	} else
		window.SetAll();

	TagType sort = TAG_NUM_OF_ITEM_TYPES;
	bool descending = false;
	if (args.size >= 2 && StringIsEqual(args[args.size - 2], "sort")) {
		const char *s = args.back();
		if (*s == '-') {
			descending = true;
			++s;
		}

		sort = ParseSortTag(s);

		args.pop_back();
		args.pop_back();
	}

	SongFilter filter;
	try {
		filter.Parse(args, fold_case);
	} catch (...) {
		r.Error(ACK_ERROR_ARG,
			GetFullMessage(std::current_exception()).c_str());
		return CommandResult::ERROR;
	}

	const DatabaseSelection selection("", true, &filter);

	db_selection_print(r, client.GetPartition(),
			   selection, true, false,
			   sort, descending,
			   window.start, window.end);
	return CommandResult::OK;
}

CommandResult
handle_find(Client &client, Request args, Response &r)
{
	return handle_match(client, args, r, false);
}

CommandResult
handle_search(Client &client, Request args, Response &r)
{
	return handle_match(client, args, r, true);
}

static CommandResult
handle_match_add(Client &client, Request args, Response &r, bool fold_case)
{
	SongFilter filter;
	try {
		filter.Parse(args, fold_case);
	} catch (...) {
		r.Error(ACK_ERROR_ARG,
			GetFullMessage(std::current_exception()).c_str());
		return CommandResult::ERROR;
	}

	auto &partition = client.GetPartition();
	const ScopeBulkEdit bulk_edit(partition);

	const DatabaseSelection selection("", true, &filter);
	AddFromDatabase(partition, selection);
	return CommandResult::OK;
}

CommandResult
handle_findadd(Client &client, Request args, Response &r)
{
	return handle_match_add(client, args, r, false);
}

CommandResult
handle_searchadd(Client &client, Request args, Response &r)
{
	return handle_match_add(client, args, r, true);
}

CommandResult
handle_searchaddpl(Client &client, Request args, Response &r)
{
	const char *playlist = args.shift();

	SongFilter filter;
	try {
		filter.Parse(args, true);
	} catch (...) {
		r.Error(ACK_ERROR_ARG,
			GetFullMessage(std::current_exception()).c_str());
		return CommandResult::ERROR;
	}

	const Database &db = client.GetDatabaseOrThrow();

	search_add_to_playlist(db, client.GetStorage(),
			       "", playlist, &filter);
	return CommandResult::OK;
}

CommandResult
handle_count(Client &client, Request args, Response &r)
{
	TagType group = TAG_NUM_OF_ITEM_TYPES;
	if (args.size >= 2 && StringIsEqual(args[args.size - 2], "group")) {
		const char *s = args[args.size - 1];
		group = tag_name_parse_i(s);
		if (group == TAG_NUM_OF_ITEM_TYPES) {
			r.FormatError(ACK_ERROR_ARG,
				      "Unknown tag type: %s", s);
			return CommandResult::ERROR;
		}

		args.pop_back();
		args.pop_back();
	}

	SongFilter filter;
	if (!args.empty()) {
		try {
			filter.Parse(args, false);
		} catch (...) {
			r.Error(ACK_ERROR_ARG,
				GetFullMessage(std::current_exception()).c_str());
			return CommandResult::ERROR;
		}
	}

	PrintSongCount(r, client.GetPartition(), "", &filter, group);
	return CommandResult::OK;
}

CommandResult
handle_listall(Client &client, Request args, Response &r)
{
	/* default is root directory */
	const auto uri = args.GetOptional(0, "");

	db_selection_print(r, client.GetPartition(),
			   DatabaseSelection(uri, true),
			   false, false);
	return CommandResult::OK;
}

static CommandResult
handle_list_file(Client &client, Request args, Response &r)
{
	std::unique_ptr<SongFilter> filter;

	if (!args.empty()) {
		filter.reset(new SongFilter());
		try {
			filter->Parse(args, false);
		} catch (...) {
			r.Error(ACK_ERROR_ARG,
				GetFullMessage(std::current_exception()).c_str());
			return CommandResult::ERROR;
		}
	}

	PrintSongUris(r, client.GetPartition(), filter.get());
	return CommandResult::OK;
}

CommandResult
handle_list(Client &client, Request args, Response &r)
{
	const char *tag_name = args.shift();
	if (StringEqualsCaseASCII(tag_name, "file") ||
	    StringEqualsCaseASCII(tag_name, "filename"))
		return handle_list_file(client, args, r);

	const auto tagType = tag_name_parse_i(tag_name);
	if (tagType == TAG_NUM_OF_ITEM_TYPES) {
		r.FormatError(ACK_ERROR_ARG,
			      "Unknown tag type: %s", tag_name);
		return CommandResult::ERROR;
	}

	std::unique_ptr<SongFilter> filter;
	TagMask group_mask = TagMask::None();

	if (args.size == 1) {
		/* for compatibility with < 0.12.0 */
		if (tagType != TAG_ALBUM) {
			r.FormatError(ACK_ERROR_ARG,
				      "should be \"%s\" for 3 arguments",
				      tag_item_names[TAG_ALBUM]);
			return CommandResult::ERROR;
		}

		filter.reset(new SongFilter(TAG_ARTIST,
					    args.shift()));
	}

	while (args.size >= 2 &&
	       StringIsEqual(args[args.size - 2], "group")) {
		const char *s = args[args.size - 1];
		TagType gt = tag_name_parse_i(s);
		if (gt == TAG_NUM_OF_ITEM_TYPES) {
			r.FormatError(ACK_ERROR_ARG,
				      "Unknown tag type: %s", s);
			return CommandResult::ERROR;
		}

		group_mask |= gt;

		args.pop_back();
		args.pop_back();
	}

	if (!args.empty()) {
		filter.reset(new SongFilter());
		try {
			filter->Parse(args, false);
		} catch (...) {
			r.Error(ACK_ERROR_ARG,
				GetFullMessage(std::current_exception()).c_str());
			return CommandResult::ERROR;
		}
	}

	if (group_mask.Test(tagType)) {
		r.Error(ACK_ERROR_ARG, "Conflicting group");
		return CommandResult::ERROR;
	}

	PrintUniqueTags(r, client.GetPartition(),
			tagType, group_mask, filter.get());
	return CommandResult::OK;
}

CommandResult
handle_listallinfo(Client &client, Request args, Response &r)
{
	/* default is root directory */
	const auto uri = args.GetOptional(0, "");

	db_selection_print(r, client.GetPartition(),
			   DatabaseSelection(uri, true),
			   true, false);
	return CommandResult::OK;
}
