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
#include "PlayerCommands.hxx"
#include "Request.hxx"
#include "CommandError.hxx"
#include "queue/Playlist.hxx"
#include "PlaylistPrint.hxx"
#include "client/Client.hxx"
#include "client/Response.hxx"
#include "mixer/Volume.hxx"
#include "Partition.hxx"
#include "Instance.hxx"
#include "Idle.hxx"
#include "AudioFormat.hxx"
#include "util/StringBuffer.hxx"
#include "util/ScopeExit.hxx"
#include "util/Exception.hxx"

#ifdef ENABLE_DATABASE
#include "db/update/Service.hxx"
#endif

#define COMMAND_STATUS_STATE            "state"
#define COMMAND_STATUS_REPEAT           "repeat"
#define COMMAND_STATUS_SINGLE           "single"
#define COMMAND_STATUS_CONSUME          "consume"
#define COMMAND_STATUS_RANDOM           "random"
#define COMMAND_STATUS_PLAYLIST         "playlist"
#define COMMAND_STATUS_PLAYLIST_LENGTH  "playlistlength"
#define COMMAND_STATUS_SONG             "song"
#define COMMAND_STATUS_SONGID           "songid"
#define COMMAND_STATUS_NEXTSONG         "nextsong"
#define COMMAND_STATUS_NEXTSONGID       "nextsongid"
#define COMMAND_STATUS_TIME             "time"
#define COMMAND_STATUS_BITRATE          "bitrate"
#define COMMAND_STATUS_ERROR            "error"
#define COMMAND_STATUS_CROSSFADE	"xfade"
#define COMMAND_STATUS_MIXRAMPDB	"mixrampdb"
#define COMMAND_STATUS_MIXRAMPDELAY	"mixrampdelay"
#define COMMAND_STATUS_AUDIO		"audio"
#define COMMAND_STATUS_UPDATING_DB	"updating_db"

CommandResult
handle_play(Client &client, Request args, gcc_unused Response &r)
{
	int song = args.ParseOptional(0, -1);

	client.GetPartition().PlayPosition(song);
	return CommandResult::OK;
}

CommandResult
handle_playid(Client &client, Request args, gcc_unused Response &r)
{
	int id = args.ParseOptional(0, -1);

	client.GetPartition().PlayId(id);
	return CommandResult::OK;
}

CommandResult
handle_stop(Client &client, gcc_unused Request args, gcc_unused Response &r)
{
	client.GetPartition().Stop();
	return CommandResult::OK;
}

CommandResult
handle_currentsong(Client &client, gcc_unused Request args, Response &r)
{
	playlist_print_current(r, client.GetPlaylist());
	return CommandResult::OK;
}

CommandResult
handle_pause(Client &client, Request args, gcc_unused Response &r)
{
	auto &pc = client.GetPlayerControl();

	if (!args.IsEmpty()) {
		bool pause_flag = args.ParseBool(0);
		pc.LockSetPause(pause_flag);
	} else
		pc.LockPause();

	return CommandResult::OK;
}

CommandResult
handle_status(Client &client, gcc_unused Request args, Response &r)
{
	auto &pc = client.GetPlayerControl();

	const char *state = nullptr;
	int song;

	const auto player_status = pc.LockGetStatus();

	switch (player_status.state) {
	case PlayerState::STOP:
		state = "stop";
		break;
	case PlayerState::PAUSE:
		state = "pause";
		break;
	case PlayerState::PLAY:
		state = "play";
		break;
	}

	const playlist &playlist = client.GetPlaylist();
	r.Format("volume: %i\n"
		 COMMAND_STATUS_REPEAT ": %i\n"
		 COMMAND_STATUS_RANDOM ": %i\n"
		 COMMAND_STATUS_SINGLE ": %i\n"
		 COMMAND_STATUS_CONSUME ": %i\n"
		 COMMAND_STATUS_PLAYLIST ": %li\n"
		 COMMAND_STATUS_PLAYLIST_LENGTH ": %i\n"
		 COMMAND_STATUS_MIXRAMPDB ": %f\n"
		 COMMAND_STATUS_STATE ": %s\n",
		 volume_level_get(client.GetPartition().outputs),
		 playlist.GetRepeat(),
		 playlist.GetRandom(),
		 playlist.GetSingle(),
		 playlist.GetConsume(),
		 (unsigned long)playlist.GetVersion(),
		 playlist.GetLength(),
		 pc.GetMixRampDb(),
		 state);

	if (pc.GetCrossFade() > 0)
		r.Format(COMMAND_STATUS_CROSSFADE ": %i\n",
			 int(pc.GetCrossFade() + 0.5));

	if (pc.GetMixRampDelay() > 0)
		r.Format(COMMAND_STATUS_MIXRAMPDELAY ": %f\n",
			 pc.GetMixRampDelay());

	song = playlist.GetCurrentPosition();
	if (song >= 0) {
		r.Format(COMMAND_STATUS_SONG ": %i\n"
			 COMMAND_STATUS_SONGID ": %u\n",
			 song, playlist.PositionToId(song));
	}

	if (player_status.state != PlayerState::STOP) {
		r.Format(COMMAND_STATUS_TIME ": %i:%i\n"
			 "elapsed: %1.3f\n"
			 COMMAND_STATUS_BITRATE ": %u\n",
			 player_status.elapsed_time.RoundS(),
			 player_status.total_time.IsNegative()
			 ? 0u
			 : unsigned(player_status.total_time.RoundS()),
			 player_status.elapsed_time.ToDoubleS(),
			 player_status.bit_rate);

		if (!player_status.total_time.IsNegative())
			r.Format("duration: %1.3f\n",
				 player_status.total_time.ToDoubleS());

		if (player_status.audio_format.IsDefined())
			r.Format(COMMAND_STATUS_AUDIO ": %s\n",
				 ToString(player_status.audio_format).c_str());
	}

#ifdef ENABLE_DATABASE
	const UpdateService *update_service = client.GetInstance().update;
	unsigned updateJobId = update_service != nullptr
		? update_service->GetId()
		: 0;
	if (updateJobId != 0) {
		r.Format(COMMAND_STATUS_UPDATING_DB ": %i\n",
			 updateJobId);
	}
#endif

	try {
		pc.LockCheckRethrowError();
	} catch (...) {
		r.Format(COMMAND_STATUS_ERROR ": %s\n",
			 GetFullMessage(std::current_exception()).c_str());
	}

	song = playlist.GetNextPosition();
	if (song >= 0)
		r.Format(COMMAND_STATUS_NEXTSONG ": %i\n"
			 COMMAND_STATUS_NEXTSONGID ": %u\n",
			 song, playlist.PositionToId(song));

	return CommandResult::OK;
}

CommandResult
handle_next(Client &client, gcc_unused Request args, gcc_unused Response &r)
{
	playlist &playlist = client.GetPlaylist();

	/* single mode is not considered when this is user who
	 * wants to change song. */
	const bool single = playlist.queue.single;
	playlist.queue.single = false;

	AtScopeExit(&playlist, single) {
		playlist.queue.single = single;
	};

	client.GetPartition().PlayNext();
	return CommandResult::OK;
}

CommandResult
handle_previous(Client &client, gcc_unused Request args,
		gcc_unused Response &r)
{
	client.GetPartition().PlayPrevious();
	return CommandResult::OK;
}

CommandResult
handle_repeat(Client &client, Request args, gcc_unused Response &r)
{
	bool status = args.ParseBool(0);
	client.GetPartition().SetRepeat(status);
	return CommandResult::OK;
}

CommandResult
handle_single(Client &client, Request args, gcc_unused Response &r)
{
	bool status = args.ParseBool(0);
	client.GetPartition().SetSingle(status);
	return CommandResult::OK;
}

CommandResult
handle_consume(Client &client, Request args, gcc_unused Response &r)
{
	bool status = args.ParseBool(0);
	client.GetPartition().SetConsume(status);
	return CommandResult::OK;
}

CommandResult
handle_random(Client &client, Request args, gcc_unused Response &r)
{
	bool status = args.ParseBool(0);
	auto &partition = client.GetPartition();
	partition.SetRandom(status);
	partition.UpdateEffectiveReplayGainMode();
	return CommandResult::OK;
}

CommandResult
handle_clearerror(Client &client, gcc_unused Request args,
		  gcc_unused Response &r)
{
	client.GetPlayerControl().LockClearError();
	return CommandResult::OK;
}

CommandResult
handle_seek(Client &client, Request args, gcc_unused Response &r)
{
	unsigned song = args.ParseUnsigned(0);
	SongTime seek_time = args.ParseSongTime(1);

	client.GetPartition().SeekSongPosition(song, seek_time);
	return CommandResult::OK;
}

CommandResult
handle_seekid(Client &client, Request args, gcc_unused Response &r)
{
	unsigned id = args.ParseUnsigned(0);
	SongTime seek_time = args.ParseSongTime(1);

	client.GetPartition().SeekSongId(id, seek_time);
	return CommandResult::OK;
}

CommandResult
handle_seekcur(Client &client, Request args, gcc_unused Response &r)
{
	const char *p = args.front();
	bool relative = *p == '+' || *p == '-';
	SignedSongTime seek_time = ParseCommandArgSignedSongTime(p);

	client.GetPartition().SeekCurrent(seek_time, relative);
	return CommandResult::OK;
}

CommandResult
handle_crossfade(Client &client, Request args, gcc_unused Response &r)
{
	unsigned xfade_time = args.ParseUnsigned(0);
	client.GetPlayerControl().SetCrossFade(xfade_time);
	return CommandResult::OK;
}

CommandResult
handle_mixrampdb(Client &client, Request args, gcc_unused Response &r)
{
	float db = args.ParseFloat(0);
	client.GetPlayerControl().SetMixRampDb(db);
	return CommandResult::OK;
}

CommandResult
handle_mixrampdelay(Client &client, Request args, gcc_unused Response &r)
{
	float delay_secs = args.ParseFloat(0);
	client.GetPlayerControl().SetMixRampDelay(delay_secs);
	return CommandResult::OK;
}

CommandResult
handle_replay_gain_mode(Client &client, Request args, Response &)
{
	auto new_mode = FromString(args.front());
	auto &partition = client.GetPartition();
	partition.SetReplayGainMode(new_mode);
	partition.EmitIdle(IDLE_OPTIONS);
	return CommandResult::OK;
}

CommandResult
handle_replay_gain_status(Client &client, gcc_unused Request args,
			  Response &r)
{
	r.Format("replay_gain_mode: %s\n",
		 ToString(client.GetPartition().replay_gain_mode));
	return CommandResult::OK;
}
