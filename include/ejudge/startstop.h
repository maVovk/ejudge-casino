/* -*- c -*- */

#ifndef __STARTSTOP_H__
#define __STARTSTOP_H__

/* Copyright (C) 2006-2023 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

enum
{
  START_RESTART = 1,
  START_STOP,
  START_ROTATE,
};

void start_set_self_args(int argc, char *argv[]);
void start_set_args(char *argv[]);
int start_switch_user(const unsigned char *user, const unsigned char *group);

int start_prepare(const unsigned char *user, const unsigned char *group,
                  const unsigned char *workdir);
void start_restart(void);

int
start_find_process(const unsigned char *name, int *p_uid);
int
start_find_all_processes(const unsigned char *name, int **p_pids);

int start_kill(int pid, int op);
int start_daemon(const unsigned char *log_path);
int start_open_log(const unsigned char *log_path);

void start_shutdown(const unsigned char *cmd) __attribute__((noreturn));

#endif /* __STARTSTOP_H__ */
