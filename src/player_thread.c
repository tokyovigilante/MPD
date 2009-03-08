/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "player_thread.h"
#include "player_control.h"
#include "decoder_control.h"
#include "decoder_thread.h"
#include "output_all.h"
#include "pcm_volume.h"
#include "path.h"
#include "event_pipe.h"
#include "crossfade.h"
#include "song.h"
#include "tag.h"
#include "pipe.h"
#include "chunk.h"
#include "idle.h"
#include "main.h"
#include "buffer.h"

#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "player_thread"

enum xfade_state {
	XFADE_DISABLED = -1,
	XFADE_UNKNOWN = 0,
	XFADE_ENABLED = 1
};

struct player {
	struct music_buffer *buffer;
	struct music_pipe *pipe;

	/**
	 * are we waiting for buffered_before_play?
	 */
	bool buffering;

	/**
	 * true if the decoder is starting and did not provide data
	 * yet
	 */
	bool decoder_starting;

	/**
	 * is the player paused?
	 */
	bool paused;

	/**
	 * is there a new song in pc.next_song?
	 */
	bool queued;

	/**
	 * the song currently being played
	 */
	struct song *song;

	/**
	 * is cross fading enabled?
	 */
	enum xfade_state xfade;

	/**
	 * The current audio format for the audio outputs.
	 */
	struct audio_format play_audio_format;

	/**
	 * Coefficient for converting a PCM buffer size into a time
	 * span.
	 */
	double size_to_time;
};

static void player_command_finished(void)
{
	assert(pc.command != PLAYER_COMMAND_NONE);

	pc.command = PLAYER_COMMAND_NONE;
	notify_signal(&main_notify);
}

static void
player_dc_stop(struct player *player)
{
	dc_stop(&pc.notify);

	if (dc.pipe != NULL) {
		music_pipe_clear(dc.pipe, player->buffer);

		if (dc.pipe != player->pipe)
			music_pipe_free(dc.pipe);

		dc.pipe = NULL;
	}
}

static void player_stop_decoder(void)
{
	dc_stop(&pc.notify);
	pc.state = PLAYER_STATE_STOP;
	event_pipe_emit(PIPE_EVENT_PLAYLIST);
}

static bool
player_wait_for_decoder(struct player *player)
{
	dc_command_wait(&pc.notify);

	if (decoder_has_failed()) {
		assert(dc.next_song == NULL || dc.next_song->url != NULL);
		pc.errored_song = dc.next_song;
		pc.error = PLAYER_ERROR_FILE;
		pc.next_song = NULL;
		return false;
	}

	pc.total_time = pc.next_song->tag != NULL
		? pc.next_song->tag->time : 0;
	pc.bit_rate = 0;
	audio_format_clear(&pc.audio_format);

	player->song = pc.next_song;
	pc.next_song = NULL;
	pc.elapsed_time = 0;
	player->queued = false;
	player->decoder_starting = true;

	/* call syncPlaylistWithQueue() in the main thread */
	event_pipe_emit(PIPE_EVENT_PLAYLIST);

	return true;
}

static bool
player_check_decoder_startup(struct player *player)
{
	assert(player->decoder_starting);

	if (decoder_has_failed()) {
		/* the decoder failed */
		assert(dc.next_song == NULL || dc.next_song->url != NULL);

		pc.errored_song = dc.next_song;
		pc.error = PLAYER_ERROR_FILE;

		return false;
	} else if (!decoder_is_starting()) {
		/* the decoder is ready and ok */
		player->decoder_starting = false;

		if (!audio_output_all_open(&dc.out_audio_format)) {
			char *uri = song_get_uri(dc.next_song);
			g_warning("problems opening audio device "
				  "while playing \"%s\"", uri);
			g_free(uri);

			assert(dc.next_song == NULL || dc.next_song->url != NULL);
			pc.errored_song = dc.next_song;
			pc.error = PLAYER_ERROR_AUDIO;
			return false;
		}

		if (player->paused)
			audio_output_all_close();

		pc.total_time = dc.total_time;
		pc.audio_format = dc.in_audio_format;
		player->play_audio_format = dc.out_audio_format;
		player->size_to_time =
			audioFormatSizeToTime(&dc.out_audio_format);

		return true;
	} else {
		/* the decoder is not yet ready; wait
		   some more */
		notify_wait(&pc.notify);

		return true;
	}
}

static bool player_seek_decoder(struct player *player)
{
	double where;
	bool ret;

	if (decoder_current_song() != pc.next_song) {
		player_dc_stop(player);

		music_pipe_clear(player->pipe, player->buffer);
		dc.pipe = player->pipe;
		dc_start_async(pc.next_song);

		ret = player_wait_for_decoder(player);
		if (!ret)
			return false;
	} else {
		pc.next_song = NULL;
		player->queued = false;
	}

	where = pc.seek_where;
	if (where > pc.total_time)
		where = pc.total_time - 0.1;
	if (where < 0.0)
		where = 0.0;

	ret = dc_seek(&pc.notify, where);
	if (ret)
		pc.elapsed_time = where;

	player_command_finished();

	return ret;
}

static void player_process_command(struct player *player)
{
	switch (pc.command) {
	case PLAYER_COMMAND_NONE:
	case PLAYER_COMMAND_PLAY:
	case PLAYER_COMMAND_STOP:
	case PLAYER_COMMAND_EXIT:
	case PLAYER_COMMAND_CLOSE_AUDIO:
		break;

	case PLAYER_COMMAND_QUEUE:
		assert(pc.next_song != NULL);
		assert(!player->queued);
		assert(dc.pipe == NULL || dc.pipe == player->pipe);

		player->queued = true;
		player_command_finished();
		break;

	case PLAYER_COMMAND_PAUSE:
		player->paused = !player->paused;
		if (player->paused) {
			audio_output_all_pause();
			pc.state = PLAYER_STATE_PAUSE;
		} else {
			if (audio_output_all_open(NULL)) {
				pc.state = PLAYER_STATE_PLAY;
			} else {
				assert(dc.next_song == NULL || dc.next_song->url != NULL);
				pc.errored_song = dc.next_song;
				pc.error = PLAYER_ERROR_AUDIO;

				player->paused = true;
			}
		}
		player_command_finished();
		break;

	case PLAYER_COMMAND_SEEK:
		if (player_seek_decoder(player)) {
			player->xfade = XFADE_UNKNOWN;

			/* abort buffering when the user has requested
			   a seek */
			player->buffering = false;

			audio_output_all_cancel();
		}
		break;

	case PLAYER_COMMAND_CANCEL:
		if (pc.next_song == NULL) {
			/* the cancel request arrived too later, we're
			   already playing the queued song...  stop
			   everything now */
			pc.command = PLAYER_COMMAND_STOP;
			return;
		}

		if (dc.pipe != NULL && dc.pipe != player->pipe) {
			/* the decoder is already decoding the song -
			   stop it and reset the position */
			player_dc_stop(player);
			dc_stop(&pc.notify);
		}

		pc.next_song = NULL;
		player->queued = false;
		player_command_finished();
		break;
	}
}

static bool
play_chunk(struct song *song, struct music_chunk *chunk,
	   const struct audio_format *format, double sizeToTime)
{
	bool success;

	assert(music_chunk_check_format(chunk, format));

	pc.elapsed_time = chunk->times;
	pc.bit_rate = chunk->bit_rate;

	if (chunk->tag != NULL) {
		audio_output_all_tag(chunk->tag);

		if (!song_is_file(song)) {
			/* always update the tag of remote streams */
			struct tag *old_tag = song->tag;

			song->tag = tag_dup(chunk->tag);

			if (old_tag != NULL)
				tag_free(old_tag);

			/* the main thread will update the playlist
			   version when he receives this event */
			event_pipe_emit(PIPE_EVENT_TAG);

			/* notify all clients that the tag of the
			   current song has changed */
			idle_add(IDLE_PLAYER);
		}
	}

	if (chunk->length == 0)
		return true;

	success = pcm_volume(chunk->data, chunk->length,
			     format, pc.software_volume);
	if (!success) {
		g_warning("pcm_volume() failed on %u:%u:%u",
			  format->sample_rate, format->bits, format->channels);
		pc.errored_song = dc.current_song;
		pc.error = PLAYER_ERROR_AUDIO;
		return false;
	}

	if (!audio_output_all_play(chunk->data, chunk->length)) {
		pc.errored_song = dc.current_song;
		pc.error = PLAYER_ERROR_AUDIO;
		return false;
	}

	pc.total_play_time += sizeToTime * chunk->length;
	return true;
}

static void do_play(void)
{
	struct player player = {
		.buffering = true,
		.decoder_starting = false,
		.paused = false,
		.queued = false,
		.song = NULL,
		.xfade = XFADE_UNKNOWN,
		.size_to_time = 0.0,
	};
	unsigned int crossFadeChunks = 0;
	/** has cross-fading begun? */
	bool cross_fading = false;
	static const char silence[CHUNK_SIZE];

	player.buffer = music_buffer_new(pc.buffer_chunks);
	player.pipe = music_pipe_new();

	dc.buffer = player.buffer;
	dc.pipe = player.pipe;
	dc_start(&pc.notify, pc.next_song);
	if (!player_wait_for_decoder(&player)) {
		player_stop_decoder();
		player_command_finished();
		music_pipe_free(player.pipe);
		music_buffer_free(player.buffer);
		return;
	}

	pc.elapsed_time = 0;
	pc.state = PLAYER_STATE_PLAY;
	player_command_finished();

	while (true) {
		player_process_command(&player);
		if (pc.command == PLAYER_COMMAND_STOP ||
		    pc.command == PLAYER_COMMAND_EXIT ||
		    pc.command == PLAYER_COMMAND_CLOSE_AUDIO) {
			audio_output_all_cancel();
			break;
		}

		if (player.buffering) {
			if (music_pipe_size(player.pipe) < pc.buffered_before_play &&
			    !decoder_is_idle()) {
				/* not enough decoded buffer space yet */
				notify_wait(&pc.notify);
				continue;
			} else {
				/* buffering is complete */
				player.buffering = false;
			}
		}

		if (player.decoder_starting) {
			bool success;

			success = player_check_decoder_startup(&player);
			if (!success)
				break;
			continue;
		}

#ifndef NDEBUG
		/*
		music_pipe_check_format(&play_audio_format,
					player.next_song_chunk,
					&dc.out_audio_format);
		*/
#endif

		if (decoder_is_idle() && player.queued) {
			/* the decoder has finished the current song;
			   make it decode the next song */
			assert(pc.next_song != NULL);
			assert(dc.pipe == NULL || dc.pipe == player.pipe);

			player.queued = false;
			dc.pipe = music_pipe_new();
			dc_start_async(pc.next_song);
		}

		if (dc.pipe != NULL && dc.pipe != player.pipe &&
		    player.xfade == XFADE_UNKNOWN &&
		    !decoder_is_starting()) {
			/* enable cross fading in this song?  if yes,
			   calculate how many chunks will be required
			   for it */
			crossFadeChunks =
				cross_fade_calc(pc.cross_fade_seconds, dc.total_time,
						&dc.out_audio_format,
						&player.play_audio_format,
						music_buffer_size(player.buffer) -
						pc.buffered_before_play);
			if (crossFadeChunks > 0) {
				player.xfade = XFADE_ENABLED;
				cross_fading = false;
			} else
				/* cross fading is disabled or the
				   next song is too short */
				player.xfade = XFADE_DISABLED;
		}

		if (player.paused)
			notify_wait(&pc.notify);
		else if (music_pipe_size(player.pipe) > 0) {
			struct music_chunk *chunk = NULL;
			unsigned int fadePosition;
			bool success;

			if (player.xfade == XFADE_ENABLED &&
			    dc.pipe != NULL && dc.pipe != player.pipe &&
			    (fadePosition = music_pipe_size(player.pipe))
			    <= crossFadeChunks) {
				/* perform cross fade */
				struct music_chunk *other_chunk =
					music_pipe_shift(dc.pipe);

				if (!cross_fading) {
					/* beginning of the cross fade
					   - adjust crossFadeChunks
					   which might be bigger than
					   the remaining number of
					   chunks in the old song */
					crossFadeChunks = fadePosition;
					cross_fading = true;
				}

				if (other_chunk != NULL) {
					chunk = music_pipe_shift(player.pipe);
					cross_fade_apply(chunk, other_chunk,
							 &dc.out_audio_format,
							 fadePosition,
							 crossFadeChunks);
					music_buffer_return(player.buffer, other_chunk);
				} else {
					/* there are not enough
					   decoded chunks yet */
					if (decoder_is_idle()) {
						/* the decoder isn't
						   running, abort
						   cross fading */
						player.xfade = XFADE_DISABLED;
					} else {
						/* wait for the
						   decoder */
						notify_signal(&dc.notify);
						notify_wait(&pc.notify);

						continue;
					}
				}
			}

			if (chunk == NULL)
				chunk = music_pipe_shift(player.pipe);

			/* play the current chunk */

			success = play_chunk(player.song, chunk,
					     &player.play_audio_format,
					     player.size_to_time);
			music_buffer_return(player.buffer, chunk);

			if (!success)
				break;

			/* this formula should prevent that the
			   decoder gets woken up with each chunk; it
			   is more efficient to make it decode a
			   larger block at a time */
			if (!decoder_is_idle() &&
			    music_pipe_size(dc.pipe) <= (pc.buffered_before_play +
							 music_buffer_size(player.buffer) * 3) / 4)
				notify_signal(&dc.notify);
		} else if (dc.pipe != NULL && dc.pipe != player.pipe) {
			/* at the beginning of a new song */

			player.xfade = XFADE_UNKNOWN;

			music_pipe_free(player.pipe);
			player.pipe = dc.pipe;

			if (!player_wait_for_decoder(&player))
				break;
		} else if (decoder_is_idle()) {
			break;
		} else {
			size_t frame_size =
				audio_format_frame_size(&player.play_audio_format);
			/* this formula ensures that we don't send
			   partial frames */
			unsigned num_frames = CHUNK_SIZE / frame_size;

			/*DEBUG("waiting for decoded audio, play silence\n");*/
			if (!audio_output_all_play(silence, num_frames * frame_size))
				break;
		}
	}

	if (player.queued) {
		assert(pc.next_song != NULL);
		pc.next_song = NULL;
	}

	player_stop_decoder();

	if (dc.pipe != NULL && dc.pipe != player.pipe) {
		music_pipe_clear(dc.pipe, player.buffer);
		music_pipe_free(dc.pipe);
	}

	music_pipe_clear(player.pipe, player.buffer);
	music_pipe_free(player.pipe);

	music_buffer_free(player.buffer);
}

static gpointer player_task(G_GNUC_UNUSED gpointer arg)
{
	decoder_thread_start();

	while (1) {
		switch (pc.command) {
		case PLAYER_COMMAND_PLAY:
		case PLAYER_COMMAND_QUEUE:
			assert(pc.next_song != NULL);

			do_play();
			break;

		case PLAYER_COMMAND_STOP:
		case PLAYER_COMMAND_SEEK:
		case PLAYER_COMMAND_PAUSE:
			pc.next_song = NULL;
			player_command_finished();
			break;

		case PLAYER_COMMAND_CLOSE_AUDIO:
			audio_output_all_close();
			player_command_finished();
			break;

		case PLAYER_COMMAND_EXIT:
			dc_quit();
			audio_output_all_close();
			player_command_finished();
			g_thread_exit(NULL);
			break;

		case PLAYER_COMMAND_CANCEL:
			pc.next_song = NULL;
			player_command_finished();
			break;

		case PLAYER_COMMAND_NONE:
			notify_wait(&pc.notify);
			break;
		}
	}
	return NULL;
}

void player_create(void)
{
	GError *e = NULL;

	assert(pc.thread == NULL);

	pc.thread = g_thread_create(player_task, NULL, true, &e);
	if (pc.thread == NULL)
		g_error("Failed to spawn player task: %s", e->message);
}
