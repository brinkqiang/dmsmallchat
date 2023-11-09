/* smallchat-client.c -- Client program for smallchat-server.
 *
 * Copyright (c) 2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the project name of nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>


#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#endif

#include "chatlib.h"

/* ============================================================================
 * Low level terminal handling.
 * ========================================================================== */

void disableRawModeAtExit(void);

#ifdef _WIN32

/* Windows specific raw mode implementation. */
int setRawMode(int fd, int enable) {
    /* We have a bit of global state (but local in scope) here.
     * This is needed to correctly set/undo raw mode. */
    static HANDLE orig_con_handle = INVALID_HANDLE_VALUE; // Save original console handle.
    static int atexit_registered = 0;   // Avoid registering atexit() many times.
    static int rawmode_is_set = 0;      // True if raw mode was enabled.

    if (enable) {
        /* Enable raw mode. */
        if (!atexit_registered) {
            atexit(disableRawModeAtExit);
            atexit_registered = 1;
        }
        if (rawmode_is_set) return 0; // The console is already in raw mode.

        HANDLE con_handle = GetStdHandle(STD_INPUT_HANDLE);
        if (con_handle == INVALID_HANDLE_VALUE || !GetConsoleMode(con_handle, &orig_con_handle)) {
            errno = ENOTTY;
            return -1;
        }
        DWORD mode = orig_con_handle;
        if (!SetConsoleMode(con_handle, mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))) {
            errno = ENOTTY;
            return -1;
        }
        orig_con_handle = con_handle;
        rawmode_is_set = 1;
    } else {
        /* Disable raw mode. */
        if (!rawmode_is_set) return 0; // The console is not in raw mode.
        HANDLE con_handle = orig_con_handle;
        DWORD mode = orig_con_handle;
        if (!SetConsoleMode(con_handle, mode)) {
            errno = ENOTTY;
            return -1;
        }
        rawmode_is_set = 0;
    }

    return 0;
}


#else

/* Unix specific raw mode implementation. */
int setRawMode(int fd, int enable) {
    /* We have a bit of global state (but local in scope) here.
     * This is needed to correctly set/undo raw mode. */
    static struct termios orig_termios;
    static int atexit_registered = 0;
    static int rawmode_is_set = 0;

    if (enable) {
        /* Enable raw mode. */
        if (!atexit_registered) {
            atexit(disableRawModeAtExit);
            atexit_registered = 1;
        }
        if (rawmode_is_set) return 0; // The terminal is already in raw mode.
        if (tcgetattr(fd, &orig_termios) == -1) {
            errno = ENOTTY;
            return -1;
        }
        struct termios raw = orig_termios;
        cfmakeraw(&raw);
        if (tcsetattr(fd, TCSANOW, &raw) == -1) {
            errno = ENOTTY;
            return -1;
        }
        rawmode_is_set = 1;
    } else {
        /* Disable raw mode. */
        if (!rawmode_is_set) return 0; // The terminal is not in raw mode.
        if (tcsetattr(fd, TCSANOW, &orig_termios) == -1) {
            errno = ENOTTY;
            return -1;
        }
        rawmode_is_set = 0;
    }

    return 0;
}

#endif
/* At exit we'll try to fix the terminal to the initial conditions. */
void disableRawModeAtExit(void) {
#ifdef _WIN32
    setRawMode(_fileno(stdin),0);
#else
    setRawMode(STDIN_FILENO,0);
#endif

}

/* ============================================================================
 * Mininal line editing.
 * ========================================================================== */

void terminalCleanCurrentLine(void) {
    write(fileno(stdout),"\e[2K",4);
}

void terminalCursorAtLineStart(void) {
    write(fileno(stdout),"\r",1);
}

#define IB_MAX 128
struct InputBuffer {
    char buf[IB_MAX];       // Buffer holding the data.
    int len;                // Current length.
};

/* inputBuffer*() return values: */
#define IB_ERR 0        // Sorry, unable to comply.
#define IB_OK 1         // Ok, got the new char, did the operation, ...
#define IB_GOTLINE 2    // Hey, now there is a well formed line to read.

/* Append the specified character to the buffer. */
int inputBufferAppend(struct InputBuffer *ib, int c) {
    if (ib->len >= IB_MAX) return IB_ERR; // No room.

    ib->buf[ib->len] = c;
    ib->len++;
    return IB_OK;
}

void inputBufferHide(struct InputBuffer *ib);
void inputBufferShow(struct InputBuffer *ib);

/* Process every new keystroke arriving from the keyboard. As a side effect
 * the input buffer state is modified in order to reflect the current line
 * the user is typing, so that reading the input buffer 'buf' for 'len'
 * bytes will contain it. */
int inputBufferFeedChar(struct InputBuffer *ib, int c) {
    switch(c) {
    case '\n':
        break;          // Ignored. We handle \r instead.
    case '\r':
        return IB_GOTLINE;
    case 127:           // Backspace.
        if (ib->len > 0) {
            ib->len--;
            inputBufferHide(ib);
            inputBufferShow(ib);
        }
        break;
    default:
        if (inputBufferAppend(ib,c) == IB_OK)
            write(fileno(stdout),ib->buf+ib->len-1,1);
        break;
    }
    return IB_OK;
}

/* Hide the line the user is typing. */
void inputBufferHide(struct InputBuffer *ib) {
    (void)ib; // Not used var, but is conceptually part of the API.
    terminalCleanCurrentLine();
    terminalCursorAtLineStart();
}

/* Show again the current line. Usually called after InputBufferHide(). */
void inputBufferShow(struct InputBuffer *ib) {
    write(fileno(stdout),ib->buf,ib->len);
}

/* Reset the buffer to be empty. */
void inputBufferClear(struct InputBuffer *ib) {
    ib->len = 0;
    inputBufferHide(ib);
}

/* =============================================================================
 * Main program logic, finally :)
 * ========================================================================== */

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        exit(1);
    }
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        perror("Failed to initialize winsock");
        return -1;
    }
#endif
    /* Create a TCP connection with the server. */
    int s = TCPConnect(argv[1],atoi(argv[2]),0);
    if (s == -1) {
        perror("Connecting to server");
        exit(1);
    }

    /* Put the terminal in raw mode: this way we will receive every
     * single key stroke as soon as the user types it. No buffering
     * nor translation of escape sequences of any kind. */
#ifdef _WIN32
    //setRawMode(_fileno(stdin),1);
#else
    setRawMode(STDIN_FILENO,1);
#endif
    /* Wait for the standard input or the server socket to
     * have some data. */
    fd_set readfds;
#ifdef _WIN32
    int stdin_fd = _fileno(stdin);
#else
    int stdin_fd = fileno(stdin);
#endif
    struct InputBuffer ib;
    inputBufferClear(&ib);

    while(1) {
        FD_ZERO(&readfds);
        FD_SET(s, &readfds);
        FD_SET(stdin_fd, &readfds);
        int maxfd = s > stdin_fd ? s : stdin_fd;

        int num_events = select(maxfd+1, &readfds, NULL, NULL, NULL);
        if (num_events == -1) {
            perror("select() error");
            exit(1);
        } else if (num_events) {
            char buf[128]; /* Generic buffer for both code paths. */

            if (FD_ISSET(s, &readfds)) {
                /* Data from the server? */
                size_t count = read(s,buf,sizeof(buf));
                if (count <= 0) {
                    printf("Connection lost\n");
                    exit(1);
                }
                inputBufferHide(&ib);
                write(fileno(stdout),buf,count);
                inputBufferShow(&ib);
            } else if (FD_ISSET(stdin_fd, &readfds)) {
                /* Data from the user typing on the terminal? */
                size_t count = read(stdin_fd,buf,sizeof(buf));
                for (int j = 0; j < count; j++) {
                    int res = inputBufferFeedChar(&ib,buf[j]);
                    switch(res) {
                    case IB_GOTLINE:
                        inputBufferAppend(&ib,'\n');
                        inputBufferHide(&ib);
                        write(fileno(stdout),"you> ", 5);
                        write(fileno(stdout),ib.buf,ib.len);
                        send_data(s,ib.buf,ib.len);
                        inputBufferClear(&ib);
                        break;
                    case IB_OK:
                        break;
                    }
                }
            }
        }
    }

    close_socket(s);
    return 0;
}
