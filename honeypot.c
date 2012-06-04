/*
 * honeypot.c
 * 
 * Copyright (C) 2012 Jason A. Donenfeld <Jason@zx2c4.com>. All rights reserved.
 * 
 * This is telnet honeypot server. It asks the user for a username and password
 * and logs it shamelessly to a file.
 * 
 * Much of the telnet setup logic has been taken from the hilarious nyancat
 * telnet server, nyancat.c, which is Copyright 2011 by Kevin Lange.
 * 
 * This honeypot drops all privileges and chroots to /var/empty, after opening the
 * log file and binding to the telnet port.
 * 
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/*
 * telnet.h contains some #defines for the various
 * commands, escape characters, and modes for telnet.
 * (it surprises some people that telnet is, really,
 *  a protocol, and not just raw text transmission)
 */
#include "telnet.h"

FILE *input = 0;
FILE *output = 0;
FILE *logfile = 0;

/*
 * Telnet requires us to send a specific sequence
 * for a line break (\r\000\n), so let's make it happy.
 */
void newline(int n)
{
	int i;

	for (i = 0; i < n; ++i) {
		/* Send the telnet newline sequence */
		putc('\r', output);
		putc(0, output);
		putc('\n', output);
	}
}


/*
 * In the standalone mode, we want to handle an interrupt signal
 * (^C) so that we can restore the cursor and clear the terminal.
 */
void SIGINT_handler(int sig)
{
	fprintf(stderr, "Got CTRL+C, exiting gracefully.\n");
	fprintf(output, "\033[?25h\033[0m\033[H\033[2J");
	_exit(EXIT_SUCCESS);
}

/*
 * Handle the alarm which breaks us off of options
 * handling if we didn't receive a terminal.
 */
void SIGALRM_handler(int sig)
{
	alarm(0);
	fprintf(stderr, "Bad telnet negotiation, exiting.\n");
	fprintf(output, "\033[?25h\033[0m\033[H\033[2J");
	fprintf(output, "\033[1;31m*** You must connect using a real telnet client. ***\033[0m");
	newline(1);
	fflush(output);
	_exit(EXIT_FAILURE);
}

/*
 * A child has exited.
 */
void SIGCHLD_handler(int sig)
{
	int status;
	pid_t pid;
	
	pid = wait(&status);
	printf("Process %d has exited with code %d.\n", pid, WEXITSTATUS(status));
}

/*
 * Reads a line character by character for when local echo mode is turned off.
 */
void readline(char *buffer, size_t size, int password)
{
	int i;
	unsigned char c;
	
	/* We make sure to restore the cursor. */
	fprintf(output, "\033[?25h");
	fflush(output);
	
	for (i = 0; i < size - 1; ++i) {
		if (feof(input))
			_exit(EXIT_SUCCESS);
		c = getc(input);
		if (!c)
			c = getc(input);
		if (c == '\n' || c == '\r') {
			newline(1);
			break;
		} else if (c == '\b' || c == 0x7f) {
			if (!i) {
				i = -1;
				continue;
			}
			if (password) {
				fprintf(output, "\033[%dD\033[K", i);
				fflush(output);
				i = -1;
				continue;
			} else {
				fprintf(output, "\b \b");
				fflush(output);
				i -= 2;
				continue;
			}
		} else if (c == 0xff)
			_exit(EXIT_SUCCESS);
		buffer[i] = c;
		putc(password ? '*' : c, output);
		fflush(output);
	}
	buffer[i] = 0;
	
	/* And we hide it again at the end. */
	fprintf(output, "\033[?25l");
	fflush(output);
}

/*
 * These are the options we want to use as
 * a telnet server. These are set in set_options()
 */
unsigned char telnet_options[256] = { 0 };
unsigned char telnet_willack[256] = { 0 };

/*
 * These are the values we have set or
 * agreed to during our handshake.
 * These are set in send_command(...)
 */
unsigned char telnet_do_set[256]  = { 0 };
unsigned char telnet_will_set[256]= { 0 };

/*
 * Send a command (cmd) to the telnet client
 * Also does special handling for DO/DONT/WILL/WONT
 */
void send_command(int cmd, int opt)
{
	/* Send a command to the telnet client */
	if (cmd == DO || cmd == DONT) {
		/* DO commands say what the client should do. */
		if (((cmd == DO) && (telnet_do_set[opt] != DO)) || ((cmd == DONT) && (telnet_do_set[opt] != DONT))) {
			/* And we only send them if there is a disagreement */
			telnet_do_set[opt] = cmd;
			fprintf(output, "%c%c%c", IAC, cmd, opt);
		}
	} else if (cmd == WILL || cmd == WONT) {
		/* Similarly, WILL commands say what the server will do. */
		if (((cmd == WILL) && (telnet_will_set[opt] != WILL)) || ((cmd == WONT) && (telnet_will_set[opt] != WONT))) {
			/* And we only send them during disagreements */
			telnet_will_set[opt] = cmd;
			fprintf(output, "%c%c%c", IAC, cmd, opt);
		}
	} else
		/* Other commands are sent raw */
		fprintf(output, "%c%c", IAC, cmd);
	fflush(output);
}

/*
 * Set the default options for the telnet server.
 */
void set_options()
{
	int option;
	
	/* We will not echo input */
	telnet_options[ECHO] = WILL;
	/* We will set graphics modes */
	telnet_options[SGA] = WILL;
	/* We will not set new environments */
	telnet_options[NEW_ENVIRON] = WONT;
	
	/* The client should echo its own input */
	telnet_willack[ECHO] = DONT;
	/* The client can set a graphics mode */
	telnet_willack[SGA] = DO;
	/* The client should not change, but it should tell us its window size */
	telnet_willack[NAWS] = DO;
	/* The client should tell us its terminal type (very important) */
	telnet_willack[TTYPE] = DO;
	/* No linemode */
	telnet_willack[LINEMODE] = DONT;
	/* And the client can set a new environment */
	telnet_willack[NEW_ENVIRON] = DO;
	
	
	/* Let the client know what we're using */
	for (option = 0; option < sizeof(telnet_options); ++option) {
		if (telnet_options[option])
			send_command(telnet_options[option], option);
	}
	for (option = 0; option < sizeof(telnet_options); ++option) {
		if (telnet_willack[option])
			send_command(telnet_willack[option], option);
	}
}

/*
 * Negotiate the telnet options.
 */
void negotiate_telnet()
{
	/* The default terminal is ANSI */
	char term[1024] = {'a','n','s','i', 0};
	int ttype, done = 0, sb_mode = 0, do_echo = 0, terminal_width = 80, sb_len = 0;
	/* Various pieces for the telnet communication */
	char sb[1024];
	unsigned char opt, i;
	memset(sb, 0, 1024);
	
	
	/* Set the default options. */
	set_options();	

	/* We will stop handling options after one second */
	alarm(1);

	/* Let's do this */
	while (!feof(stdin) && done < 2) {
		/* Get either IAC (start command) or a regular character (break, unless in SB mode) */
		i = getc(input);
		if (i == IAC) {
			/* If IAC, get the command */
			i = getc(input);
			switch (i) {
				case SE:
					/* End of extended option mode */
					sb_mode = 0;
					if (sb[0] == TTYPE) {
						/* This was a response to the TTYPE command, meaning
						 * that this should be a terminal type */
						alarm(0);
						strncpy(term, &sb[2], sizeof(term) - 1);
						term[sizeof(term) - 1] = 0;
						++done;
					} else if (sb[0] == NAWS) {
						/* This was a response to the NAWS command, meaning
						 * that this should be a window size */
						alarm(0);
						terminal_width = sb[2];
						++done;
					}
					break;
				case NOP:
					/* No Op */
					send_command(NOP, 0);
					fflush(output);
					break;
				case WILL:
				case WONT:
					/* Will / Won't Negotiation */
					opt = getc(input);
					if (!telnet_willack[opt])
						/* We default to WONT */
						telnet_willack[opt] = WONT;
					send_command(telnet_willack[opt], opt);
					fflush(output);
					if ((i == WILL) && (opt == TTYPE)) {
						/* WILL TTYPE? Great, let's do that now! */
						fprintf(output, "%c%c%c%c%c%c", IAC, SB, TTYPE, SEND, IAC, SE);
						fflush(output);
					}
					break;
				case DO:
				case DONT:
					/* Do / Don't Negotiation */
					opt = getc(input);
					if (!telnet_options[opt])
						/* We default to DONT */
						telnet_options[opt] = DONT;
					send_command(telnet_options[opt], opt);
					if (opt == ECHO)
						do_echo = (i == DO);
					fflush(output);
					break;
				case SB:
					/* Begin Extended Option Mode */
					sb_mode = 1;
					sb_len  = 0;
					memset(sb, 0, sizeof(sb));
					break;
				case IAC: 
					/* IAC IAC? That's probably not right. */
					done = 2;
					break;
				default:
					break;
			}
		} else if (sb_mode) {
			/* Extended Option Mode -> Accept character */
			if (sb_len < (sizeof(sb) - 1))
				/* Append this character to the SB string,
				 * but only if it doesn't put us over
				 * our limit; honestly, we shouldn't hit
				 * the limit, as we're only collecting characters
				 * for a terminal type or window size, but better safe than
				 * sorry (and vulnerable).
				 */
				sb[sb_len++] = i;
		}
	}
	
	/* What shall we now do with term, ttype, do_echo, and terminal_width? */
}

/*
 * Drops us into a chroot, if possible, and drops privs.
 */
void drop_privileges()
{
	struct passwd *user;
	
	if (geteuid() == 0) {
		user = getpwnam("nobody");
		if (!user) {
			perror("getpwnam");
			exit(EXIT_FAILURE);
		}
		if (chroot("/var/empty")) {
			perror("chroot");
			exit(EXIT_FAILURE);
		}
		if (chdir("/")) {
			perror("chdir");
			exit(EXIT_FAILURE);
		}
		if (setresgid(user->pw_gid, user->pw_gid, user->pw_gid)) {
			perror("setresgid");
			exit(EXIT_FAILURE);
		}
		if (setgroups(1, &user->pw_gid)) {
			perror("setgroups");
			exit(EXIT_FAILURE);
		}
		if (setresuid(user->pw_uid, user->pw_uid, user->pw_uid)) {
			perror("setresuid");
			exit(EXIT_FAILURE);
		}
		if (!geteuid() || !getegid()) {
			fprintf(stderr, "Mysteriously still running as root... Goodbye.\n");
			exit(EXIT_FAILURE);
		}
	}
}

void handle_connection(int fd, char *ipaddr)
{
	char username[1024];
	char password[1024];
	
	input = fdopen(fd, "r");
	if (!input) {
		perror("fdopen");
		_exit(EXIT_FAILURE);
	}
	output = fdopen(fd, "w");
	if (!output) {
		perror("fdopen");
		_exit(EXIT_FAILURE);
	}
	
	/* Set the alarm handler to quit on bad telnet clients. */
	signal(SIGALRM, SIGALRM_handler);
	/* Accept ^C -> restore cursor. */
	signal(SIGINT, SIGINT_handler);
	
	negotiate_telnet();

	/* Attempt to set terminal title for various different terminals. */
	fprintf(output, "\033kWelcome to kexec.com\033\134");
	fprintf(output, "\033]1;Welcome to kexec.com\007");
	fprintf(output, "\033]2;Welcome to kexec.com\007");

	/* Clear the screen */
	fprintf(output, "\033[H\033[2J\033[?25l");
	
	fprintf(output, "                  \033[1mkexec.com Administration Console\033[0m");
	newline(3);
	fprintf(output, "This console uses \033[1;34mGoogle App Engine\033[0m for authentication. To login as");
	newline(1);
	fprintf(output, "an administrator, enter the admin account credentials. If you do not");
	newline(1);
	fprintf(output, "yet have an account on kexec, enter your Google credentials to begin.");
	newline(4);
	fflush(output);
	
	while (1) {
		fprintf(output, "\033[1;32mUsername: \033[0m");
		readline(username, sizeof(username), 0);
		fprintf(output, "\033[1;32mPassword: \033[0m");
		readline(password, sizeof(password), 1);
		newline(2);
		fflush(output);
		fprintf(logfile, "%s - %s:%s\n", ipaddr, username, password);
		fflush(logfile);
		printf("Honeypotted: %s - %s:%s\n", ipaddr, username, password);
		sleep(1);
		newline(1);
		fprintf(output, "\033[1;31mInvalid credentials. Please try again.\033[0m");
		fflush(output);
		sleep(2);
		fprintf(output, "\033[H\033[2J\033[?25l");
		fprintf(output, "                  \033[1mkexec.com Administration Console\033[0m");
		newline(2);
		if (!strchr(username, '@')) {
			fprintf(output, "\033[1;34mBe sure to include the domain in your username (e.g. @gmail.com).");
			newline(2);
		}
		fflush(output);
	}
	fclose(input);
	fclose(output);
}

int main(int argc, char *argv[])
{
	int listen_fd, connection_fd;
	char flag;
	struct sockaddr_in6 listen_addr;
	struct sockaddr_storage connection_addr;
	socklen_t connection_addr_len;
	pid_t child;
	
	if (argc != 2) {
		fprintf(stderr, "Usage: %s LOGFILE\n", argv[0]);
		return EXIT_FAILURE;
	}
	
	/* We open the log file before chrooting. */
	logfile = fopen(argv[1], "a");
	if (!logfile) {
		perror("fopen");
		return EXIT_FAILURE;
	}
	
	/* We bind to port 23 before chrooting, as well. */
	listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (listen_fd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}
	flag = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
	flag = 0;
	setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag));
	
	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin6_family = AF_INET6;
	listen_addr.sin6_addr = in6addr_any;
	listen_addr.sin6_port = htons(23);
	if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
		perror("bind");
		return EXIT_FAILURE;
	}
	if (listen(listen_fd, 5) < 0) {
		perror("listen");
		return EXIT_FAILURE;
	}
	
	/* Before accepting any connections, we chroot. */
	drop_privileges();
	
	/* Print message when child exits. */
	signal(SIGCHLD, SIGCHLD_handler);
	
	while ((connection_addr_len = sizeof(connection_addr)) &&
		(connection_fd = accept(listen_fd, (struct sockaddr *)&connection_addr, &connection_addr_len)) >= 0) {
		child = fork();
		if (child < 0) {
			perror("fork");
			continue;
		}
		if (!child) {
			char ipaddr[INET6_ADDRSTRLEN];
			memset(ipaddr, 0, sizeof(ipaddr));
			if (connection_addr.ss_family == AF_INET6)
				inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)&connection_addr)->sin6_addr), ipaddr, INET6_ADDRSTRLEN);
			else if (connection_addr.ss_family == AF_INET)
				inet_ntop(AF_INET, &(((struct sockaddr_in *)&connection_addr)->sin_addr), ipaddr, INET6_ADDRSTRLEN);
			printf("Forked process %d for connection %s.\n", getpid(), ipaddr);
			handle_connection(connection_fd, ipaddr);
		}
	}
	fclose(logfile);
	return 0;
}