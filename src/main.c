/*
Copyright (c) 2008-2010
	Lars-Dominik Braun <PromyLOPh@lavabit.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#define _POSIX_C_SOURCE 1 /* fileno() */
#define _BSD_SOURCE /* strdup() */

/* system includes */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/select.h>
#include <time.h>
#include <ctype.h>
/* open () */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
/* tcset/getattr () */
#include <termios.h>
#include <pthread.h>
#include <assert.h>

/* pandora.com library */
#include <piano.h>

#include "main.h"
#include "terminal.h"
#include "config.h"
#include "ui.h"
#include "ui_act.h"
#include "ui_readline.h"

typedef void (*BarKeyShortcutFunc_t) (BarApp_t *app, FILE *curFd);

int main (int argc, char **argv) {
	static BarApp_t app;
	/* FIXME: max path length? */
	char ctlPath[1024];
	FILE *ctlFd = NULL;
	int maxFd, selectFds[2] = {-1, -1};
	fd_set readSet;

	/* terminal attributes _before_ we started messing around with ~ECHO */
	struct termios termOrig;

	memset (&app, 0, sizeof (app));

	/* save terminal attributes, before disabling echoing */
	BarTermSave (&termOrig);

	BarTermSetEcho (0);
	BarTermSetBuffer (0);
	/* init some things */
	ao_initialize ();
	PianoInit (&app.ph);

	WaitressInit (&app.waith);
	strncpy (app.waith.host, PIANO_RPC_HOST, sizeof (app.waith.host)-1);
	strncpy (app.waith.port, PIANO_RPC_PORT, sizeof (app.waith.port)-1);

	BarSettingsInit (&app.settings);

	BarSettingsRead (&app.settings);

	BarUiMsg (MSG_NONE, "Welcome to " PACKAGE " (" VERSION ")! "
			"Press %c for a list of commands.\n",
			app.settings.keys[BAR_KS_HELP]);

	/* init fds */
	FD_ZERO(&readSet);
	selectFds[0] = fileno (stdin);
	FD_SET(selectFds[0], &readSet);
	maxFd = selectFds[0] + 1;

	BarGetXdgConfigDir (PACKAGE "/ctl", ctlPath, sizeof (ctlPath));
	/* FIXME: why is r_+_ required? */
	ctlFd = fopen (ctlPath, "r+");
	if (ctlFd != NULL) {
		selectFds[1] = fileno (ctlFd);
		FD_SET(selectFds[1], &readSet);
		/* assuming ctlFd is always > stdin */
		maxFd = selectFds[1] + 1;
		BarUiMsg (MSG_INFO, "Control fifo at %s opened\n", ctlPath);
	}

	app.userInputSettings.ctlFd = ctlFd;
	app.userInputSettings.maxFd = &maxFd;
	app.userInputSettings.readSet = &readSet;
	app.userInputSettings.selectFds = selectFds;

	run(&app);

	PianoDestroy (&app.ph);
	PianoDestroyPlaylist (app.songHistory);
	PianoDestroyPlaylist (app.playlist);
	ao_shutdown();
	BarSettingsDestroy (&app.settings);

	/* restore terminal attributes, zsh doesn't need this, bash does... */
	BarTermRestore (&termOrig);

	return 0;
}

void run(BarApp_t *app){
	pthread_t playerThread;

	loadUser(app);

	loadProxy(app);
	//TODO probably makes sense to combine loadUser and loginUser to one function
	if(!loginUser(app)){
		return;
	}

	if(!loadStations(app)){
		return;
	}

	loadStation(app);

	/* little hack, needed to signal: hey! we need a playlist, but don't
	 * free anything (there is nothing to be freed yet) */
	memset (&app->player, 0, sizeof (app->player));

	while (!app->doQuit) {
		/* song finished playing, clean up things/scrobble song */
		if (app->player.mode == PLAYER_FINISHED_PLAYBACK) {
			handlePlayerEnded(app, &playerThread);
		}

		/* check whether player finished playing and start playing new
		 * song */
		if (app->player.mode >= PLAYER_FINISHED_PLAYBACK ||
				app->player.mode == PLAYER_FREED) {
			if (app->curStation != NULL) {
				/* what's next? */
				if (app->playlist != NULL) {
					moveSongToHistory(app);
				}
				if (app->playlist == NULL) {
					loadPlaylist(app);
				}
				/* song ready to play */
				if (app->playlist != NULL) {
					playSong(app, &playerThread);
				} /* end if playlist != NULL */
			} /* end if curStation != NULL */
		}

		handleUserInput(app);

		/* show time */
		if (app->player.mode >= PLAYER_SAMPLESIZE_INITIALIZED &&
				app->player.mode < PLAYER_FINISHED_PLAYBACK) {
			outputTime(app);
		}
	}


	/* destroy everything (including the world...) */
	if (app->player.mode != PLAYER_FREED) {
		pthread_join (playerThread, NULL);
	}
	if (app->userInputSettings.ctlFd != NULL) {
		fclose (app->userInputSettings.ctlFd);
	}
}

void loadProxy(BarApp_t *app){
	/* set up proxy (control proxy for non-us citizen or global proxy for poor
	 * firewalled fellows) */
	if (app->settings.controlProxy != NULL) {
		/* control proxy overrides global proxy */
		char tmpPath[2];
		WaitressSplitUrl (app->settings.controlProxy, app->waith.proxyHost,
				sizeof (app->waith.proxyHost), app->waith.proxyPort,
				sizeof (app->waith.proxyPort), tmpPath, sizeof (tmpPath));
	} else if (app->settings.proxy != NULL && strlen (app->settings.proxy) > 0) {
		char tmpPath[2];
		WaitressSplitUrl (app->settings.proxy, app->waith.proxyHost,
				sizeof (app->waith.proxyHost), app->waith.proxyPort,
				sizeof (app->waith.proxyPort), tmpPath, sizeof (tmpPath));
	}
}

int loginUser(BarApp_t *app){
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	PianoRequestDataLogin_t reqData;
	reqData.user = app->settings.username;
	reqData.password = app->settings.password;
	reqData.step = 0;

	BarUiMsg (MSG_INFO, "Login... ");
	if (!BarUiPianoCall (app, PIANO_REQUEST_LOGIN, &reqData, &pRet,
			&wRet)) {
		return 0;
	}
	return 1;
}

void loadUser(BarApp_t *app){
	if (app->settings.username == NULL) {
		char nameBuf[100];
		BarUiMsg (MSG_QUESTION, "Username: ");
		BarReadlineStr (nameBuf, sizeof (nameBuf), 0, stdin);
		app->settings.username = strdup (nameBuf);
	}
	if (app->settings.password == NULL) {
		char passBuf[100];
		BarUiMsg (MSG_QUESTION, "Password: ");
		BarReadlineStr (passBuf, sizeof (passBuf), 1, stdin);
		app->settings.password = strdup (passBuf);
	}
}

int loadStations(BarApp_t *app){
	PianoReturn_t pRet;
	WaitressReturn_t wRet;

	BarUiMsg (MSG_INFO, "Get stations... ");
	if (!BarUiPianoCall (app, PIANO_REQUEST_GET_STATIONS, NULL, &pRet,
			&wRet)) {
		return 0;
	}
	return 1;
}

void loadStation(BarApp_t *app){
	/* try to get autostart station */
	if (app->settings.autostartStation != NULL) {
		app->curStation = PianoFindStationById (app->ph.stations,
				app->settings.autostartStation);
		if (app->curStation == NULL) {
			BarUiMsg (MSG_ERR, "Error: Autostart station not found.\n");
		}
	}
	/* no autostart? ask the user */
	if (app->curStation == NULL) {
		app->curStation = BarUiSelectStation (&(app->ph), "Select station: ",
				app->settings.sortOrder, stdin);
	}
	if (app->curStation != NULL) {
		BarUiPrintStation (app->curStation);
	}
}

void handleUserInput(BarApp_t *app){
	struct timeval selectTimeout;
	fd_set readSetCopy;
	char buf = '\0';
	/* select modifies its arguments => copy the set */
	memcpy (&readSetCopy, app->userInputSettings.readSet, sizeof (*(app->userInputSettings.readSet)));
	selectTimeout.tv_sec = 1;
	selectTimeout.tv_usec = 0;

	/* in the meantime: wait for user actions */
	if (select (*app->userInputSettings.maxFd, &readSetCopy, NULL, NULL, &selectTimeout) > 0) {
		FILE *curFd = NULL;

		if (FD_ISSET(app->userInputSettings.selectFds[0], &readSetCopy)) {
			curFd = stdin;
		} else if (app->userInputSettings.selectFds[1] != -1 && FD_ISSET(app->userInputSettings.selectFds[1], &readSetCopy)) {
			curFd = app->userInputSettings.ctlFd;
		}
		buf = fgetc (curFd);

		size_t i;
		for (i = 0; i < BAR_KS_COUNT; i++) {
			if (app->settings.keys[i] == buf) {
				static const BarKeyShortcutFunc_t idToF[] = {BarUiActHelp,
						BarUiActLoveSong, BarUiActBanSong,
						BarUiActAddMusic, BarUiActCreateStation,
						BarUiActDeleteStation, BarUiActExplain,
						BarUiActStationFromGenre, BarUiActHistory,
						BarUiActSongInfo, BarUiActAddSharedStation,
						BarUiActMoveSong, BarUiActSkipSong, BarUiActPause,
						BarUiActQuit, BarUiActRenameStation,
						BarUiActSelectStation, BarUiActTempBanSong,
						BarUiActPrintUpcoming, BarUiActSelectQuickMix,
						BarUiActDebug, BarUiActBookmark};
				idToF[i] (app, curFd);
				break;
			}
		}
	}
}

void moveSongToHistory(BarApp_t *app){
	if (app->settings.history != 0) {
		/* prepend song to history list */
		PianoSong_t *tmpSong = app->songHistory;
		app->songHistory = app->playlist;
		/* select next song */
		app->playlist = app->playlist->next;
		app->songHistory->next = tmpSong;

		/* limit history's length */
		/* start with 1, so we're stopping at n-1 and have the
		 * chance to set ->next = NULL */
		unsigned int i = 1;
		tmpSong = app->songHistory;
		while (i < app->settings.history && tmpSong != NULL) {
			tmpSong = tmpSong->next;
			++i;
		}
		/* if too many songs in history... */
		if (tmpSong != NULL) {
			PianoSong_t *delSong = tmpSong->next;
			tmpSong->next = NULL;
			if (delSong != NULL) {
				PianoDestroyPlaylist (delSong);
			}
		}
	} else {
		/* don't keep history */
		app->playlist = app->playlist->next;
	}
}

void loadPlaylist(BarApp_t *app){
	PianoReturn_t pRet;
	WaitressReturn_t wRet;
	PianoRequestDataGetPlaylist_t reqData;
	reqData.station = app->curStation;
	reqData.format = app->settings.audioFormat;

	BarUiMsg (MSG_INFO, "Receiving new playlist... ");
	if (!BarUiPianoCall (app, PIANO_REQUEST_GET_PLAYLIST,
			&reqData, &pRet, &wRet)) {
		app->curStation = NULL;
	} else {
		app->playlist = reqData.retPlaylist;
		if (app->playlist == NULL) {
			BarUiMsg (MSG_INFO, "No tracks left.\n");
			app->curStation = NULL;
		}
	}
	BarUiStartEventCmd (&app->settings, "stationfetchplaylist",
			app->curStation, app->playlist, &app->player, pRet,
			wRet);
}


void playSong(BarApp_t *app, pthread_t *playerThread){

	BarUiPrintSong (&app->settings, app->playlist, app->curStation->isQuickMix ?
			PianoFindStationById (app->ph.stations,
			app->playlist->stationId) : NULL);

	if (app->playlist->audioUrl == NULL) {
		BarUiMsg (MSG_ERR, "Invalid song url.\n");
	} else {
		/* setup player */
		memset (&app->player, 0, sizeof (app->player));

		WaitressInit (&app->player.waith);
		WaitressSetUrl (&app->player.waith, app->playlist->audioUrl);

		/* set up global proxy, player is NULLed on songfinish */
		if (app->settings.proxy != NULL) {
			char tmpPath[2];
			WaitressSplitUrl (app->settings.proxy,
					app->player.waith.proxyHost,
					sizeof (app->player.waith.proxyHost),
					app->player.waith.proxyPort,
					sizeof (app->player.waith.proxyPort), tmpPath,
					sizeof (tmpPath));
		}

		app->player.gain = app->playlist->fileGain;
		app->player.audioFormat = app->playlist->audioFormat;

		/* throw event */
		BarUiStartEventCmd (&app->settings, "songstart",
				app->curStation, app->playlist, &app->player,
				PIANO_RET_OK, WAITRESS_RET_OK);

		/* prevent race condition, mode must _not_ be FREED if
		 * thread has been started */
		app->player.mode = PLAYER_STARTING;
		/* start player */
		pthread_create (playerThread, NULL, BarPlayerThread,
				&app->player);
	} /* end if audioUrl == NULL */

}

void handlePlayerEnded(BarApp_t *app, pthread_t *playerThread){
		BarUiStartEventCmd (&app->settings, "songfinish", app->curStation,
				app->playlist, &app->player, PIANO_RET_OK, WAITRESS_RET_OK);
		/* FIXME: pthread_join blocks everything if network connection
		 * is hung up e.g. */
		void *threadRet;
		pthread_join (*playerThread, &threadRet);
		/* don't continue playback if thread reports error */
		if (threadRet != (void *) PLAYER_RET_OK) {
			app->curStation = NULL;
		}
		memset (&app->player, 0, sizeof (app->player));
}

void outputTime(BarApp_t *app){
	/* Ugly: songDuration is unsigned _long_ int! Lets hope this won't
	 * overflow */
	int songRemaining = (signed long int) (app->player.songDuration -
			app->player.songPlayed) / BAR_PLAYER_MS_TO_S_FACTOR;
	char pos = 0;
	if (songRemaining < 0) {
		/* Use plus sign if song is longer than expected */
		pos = 1;
		songRemaining = -songRemaining;
	}
	BarUiMsg (MSG_TIME, "%c%02i:%02i/%02i:%02i\r", (pos ? '+' : '-'),
			songRemaining / 60, songRemaining % 60,
			app->player.songDuration / BAR_PLAYER_MS_TO_S_FACTOR / 60,
			app->player.songDuration / BAR_PLAYER_MS_TO_S_FACTOR % 60);
}

