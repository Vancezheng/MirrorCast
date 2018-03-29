/*
 * Copyright (C) 2016 Jones Chi
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/select.h> 
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>

#define USE_FIFO 0

#define READ 0
#define WRITE 1

#define DISCOVER_PORT 53515
#define DISCOVER_MSG "hello"
#define LOCAL_SERVER_PORT 53516
#define DATA_BUF_SIZE 10240

#define DISCOVER_MSG_TEMPLATE "{\"port\":%d,\"name\":\"CsReceiver @ %s\",\"id\":\"%s\",\"width\":1280,\"height\":960,\"connect\":\"%d\"}"

#define FIFO_PATH "/tmp/cast_fifo"
#define IP_FILE "sender_ip"

#define PRINT(fmt, ...) printf("%s   "fmt, get_cur_time(), ##__VA_ARGS__)
#define ERROR(fmt, ...) printf("%s   "fmt" :%s\n", get_cur_time(), ##__VA_ARGS__, strerror(errno))

char *get_cur_time()
{
    static char s[20];
    time_t t;
    struct tm* ltime;

    time(&t);

    ltime = localtime(&t);

    strftime(s, 20, "%Y-%m-%d %H:%M:%S", ltime);

    return s;
}

pid_t popen2(char * const *command, int *infp, int *outfp)
{
    int p_stdin[2], p_stdout[2];
    pid_t pid;

    if (pipe(p_stdin) != 0 || pipe(p_stdout) != 0)
        return -1;

    pid = fork();

    if (pid < 0)
        return pid;
    else if (pid == 0)
    {
        close(p_stdin[WRITE]);
        dup2(p_stdin[READ], READ);
        close(p_stdout[READ]);
        dup2(p_stdout[WRITE], WRITE);

        execvp((const char *)*command, command);
        ERROR("execvp");
        exit(1);
    }

    if (infp == NULL)
        close(p_stdin[WRITE]);
    else
        *infp = p_stdin[WRITE];

    if (outfp == NULL)
        close(p_stdout[READ]);
    else
        *outfp = p_stdout[READ];

    return pid;
}

int setup_udp_socket() {
    int udp_sock = -1;
    int so_reuseaddr = 1;
    int pktinfo = 1;
    struct sockaddr_in broadcast_addr;

    if ((udp_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ERROR("Error when creating udp socket");
        return -1;
    }

    if (setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr)) < 0) {
        ERROR("Error when setting reuseaddr for udp socket");
        return -1;
    }

    if (setsockopt(udp_sock, IPPROTO_IP, IP_PKTINFO, &pktinfo, sizeof(pktinfo)) < 0) {
        ERROR("Error when setting pktinfo for udp socket");
        return -1;
    }

    memset((char *)&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    broadcast_addr.sin_port = htons(DISCOVER_PORT);

    if (bind(udp_sock, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
        ERROR("Error when binding broadcast port for udp socket");
        return -1;
    }
    return udp_sock;
}

int main(int argc, char* argv[])
{
    int fifo_fp = -1;
    int udp_sock = -1;
    int tcp_sock = -1;
    int so_reuseaddr = 1;
    int tcp_client_sock = -1;
    int max_sock = -1;
    int ret = -1;
    struct sockaddr_in my_addr;
    struct sockaddr_in peer_addr;
    int addr_len;
    char resp_msg_buf[512];
    char data_msg_buf[DATA_BUF_SIZE];
    int len;
    fd_set fd_r;
    int gst_in_fp = -1;
    int gst_out_fp = -1;
    pid_t gst_pid = -1;
    int just_connect = 0;
    char *gst_sink;
    int no_data_count = 0;
    int is_connected = 0;
    time_t last_read_time, now_time;
    struct timeval tv;
    int timeout = 0;
    FILE *fp;

    fflush(stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 2 || strlen(argv[1]) <= 0) {
        PRINT("Missing sink setting\n");
        return -1;
    }

    gst_sink = argv[1];
    PRINT("Using sink: %s\n", gst_sink);

#if USE_FIFO
    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) < 0) {
        ERROR("Error when creating fifo");
        return -1;
    }
#endif

#ifdef CLIENT_MODE
    if ((tcp_client_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Error when creating tcp socket");
        return -1;
    }

    memset((char *)&peer_addr, 0, sizeof(peer_addr));
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    peer_addr.sin_port = htons(LOCAL_SERVER_PORT);

    int nRecvBuf=1024*1024; //设置为32K
    setsockopt(tcp_client_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&nRecvBuf, sizeof(nRecvBuf));

    if (connect(tcp_client_sock, (const struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0) {
        ERROR("Error when connecting to remote");
        return -1;
    }
    if (send(tcp_client_sock, "mirror\n", 7, 0) < 0) {
        ERROR("Error when sending mirror command");
        return -1;
    }
    just_connect = 1;

#else
    udp_sock = setup_udp_socket();
    PRINT("udp_sock=%d\n", udp_sock);
    if ((tcp_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        ERROR("Error when creating tcp socket");
        return -1;
    }

    if (setsockopt(tcp_sock, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr)) < 0) {
        ERROR("Error when setting reuseaddr for tcp socket");
        return -1;
    }

    memset((char *)&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    my_addr.sin_port = htons(DISCOVER_PORT);

    if (bind(tcp_sock, (struct sockaddr *)&my_addr, sizeof(my_addr)) < 0) {
        ERROR("Error when binding tcp socket");
        return -1;
    }

    if (listen(tcp_sock, 3) < 0) {
        ERROR("Error when listening tcp socket");
        return -1;
    }
#endif

    for (;;) {
        // set connect timeout
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        FD_ZERO(&fd_r);
        if (tcp_client_sock < 0) {
            FD_SET(tcp_sock, &fd_r);
        }
        FD_SET(udp_sock, &fd_r);
        if (tcp_sock > udp_sock) {
            max_sock = tcp_sock;
        } else {
            max_sock = udp_sock;
        }
        if (tcp_client_sock > 0) {
            FD_SET(tcp_client_sock, &fd_r);
            if (tcp_client_sock > max_sock) {
                max_sock = tcp_client_sock;
            }
        }
        ret = select(max_sock + 1, &fd_r, NULL, NULL, &tv);
        time(&now_time);
        if (is_connected && (now_time - last_read_time >= 10)) {
            if (tcp_client_sock > 0) {
                close(tcp_client_sock);
                tcp_client_sock = -1;
            }
            if (gst_pid > 0) {
                kill(gst_pid, SIGKILL);
                waitpid(gst_pid, NULL, 0);
                gst_pid = -1;
                gst_in_fp = -1;
                gst_out_fp = -1;
            }
            if (fifo_fp > 0) {
                close(fifo_fp);
                fifo_fp = -1;
            }
            is_connected = 0;
            if (access(IP_FILE, F_OK) == 0) {
                if (remove(IP_FILE) == -1) {
                    ERROR("Error when remove file");
                }
            }
            PRINT("read timeout for 10s, close the socket and receiver\n");
        }
        //PRINT("select=%d no_data_count=%d\n", ret, no_data_count);
        switch (ret) {
            case -1:
                ERROR("error occur");
                break;
            case 0:
                timeout = 1;
            default: {
                if (FD_ISSET(udp_sock, &fd_r)) {
                    size_t aux[128 / sizeof(size_t)];
                    char broadcast_msg_buf[128];
                    struct iovec io;
                    struct msghdr msg;
                    struct cmsghdr *cmsg;
                    io.iov_base = broadcast_msg_buf;
                    io.iov_len = sizeof(broadcast_msg_buf);
                    memset(&msg, 0, sizeof(msg));
                    msg.msg_iov = &io;
                    msg.msg_iovlen = 1;
                    msg.msg_control = aux;
                    msg.msg_controllen = sizeof(aux);
                    msg.msg_flags = 0;
                    msg.msg_name = &peer_addr;
                    msg.msg_namelen = sizeof(peer_addr);
                    len = recvmsg(udp_sock, &msg, 0);
                    if (len < 0) {
                        ERROR("Error when receiving data from discover socket");
                        close(udp_sock);
                        udp_sock = setup_udp_socket();
                        break;
                    }
                    PRINT("Receive broadcast msg: %s from: %s:%d\n", broadcast_msg_buf, inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
                    if (!strncmp(broadcast_msg_buf, DISCOVER_MSG, 5)) {
                        no_data_count = 0;
                        //PRINT("Receive discover msg: %s, from: %s\n", broadcast_msg_buf, inet_ntoa(peer_addr.sin_addr));
                        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                            if (cmsg->cmsg_level == IPPROTO_IP) {
                                struct in_pktinfo *i = (struct in_pktinfo*) CMSG_DATA(cmsg);
                                //PRINT("Response discover msg with local ip: %s\n", inet_ntoa(i->ipi_spec_dst));
                                memset(resp_msg_buf, 0, sizeof(resp_msg_buf));
                                snprintf(resp_msg_buf, sizeof(resp_msg_buf), DISCOVER_MSG_TEMPLATE, DISCOVER_PORT, inet_ntoa(i->ipi_spec_dst), inet_ntoa(i->ipi_spec_dst), is_connected);
                                //PRINT("is_connected=%d\n", is_connected);
                                if (sendto(udp_sock, resp_msg_buf, strlen(resp_msg_buf), 0, (struct sockaddr *)&peer_addr, sizeof(peer_addr)) < 0) {
                                    PRINT("Error when send discover response to peer\n");
                                }
                            }
                        }
                    }
                } else if (FD_ISSET(tcp_sock, &fd_r)) {
                    if (tcp_client_sock < 0) {
                        addr_len = sizeof(peer_addr);
                        tcp_client_sock = accept(tcp_sock, (struct sockaddr *)&peer_addr, &addr_len);
                        if (tcp_client_sock < 0) {
                            PRINT("Error when accepting client\n");
                        } else {
                            just_connect = 1;
                            is_connected = 1;
                            last_read_time = now_time;
                            PRINT("Accept peer addr: %s:%d\n", inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
                            fp = fopen(IP_FILE, "w");
                            if (!fp) {
                                ERROR("Error when open file");
                            } else {
                                fprintf(fp, "%s", inet_ntoa(peer_addr.sin_addr));
                                fclose(fp);
                            }
                        }
                    } else {
                        PRINT("Could not accept client, another connection still exist\n");
                        return -1;
                    }
                } else if (tcp_client_sock > 0 && FD_ISSET(tcp_client_sock, &fd_r)) {
                    memset(data_msg_buf, 0, sizeof(data_msg_buf));
                    len = read(tcp_client_sock, data_msg_buf, sizeof(data_msg_buf));
                    //PRINT("Receive data len: %d\n", len);
                    if (len > 0) {
                        no_data_count = 0;
                        time(&last_read_time);
                    } else {
                        no_data_count++;
                    }
                    //PRINT("len=%d, no_data_count=%d\n", len, no_data_count);
                    if (len < 0 || no_data_count > 2) {
                        PRINT("Failed to receive from tcp client socket, close the socket\n");
                        close(tcp_client_sock);
                        tcp_client_sock = -1;
                        if (gst_pid > 0) {
                            kill(gst_pid, SIGKILL);
                            waitpid(gst_pid, NULL, 0);
                            gst_pid = -1;
                            gst_in_fp = -1;
                            gst_out_fp = -1;
                        }
                        if (fifo_fp > 0) {
                            close(fifo_fp);
                            fifo_fp = -1;
                        }
                        is_connected = 0;
                        if (access(IP_FILE, F_OK) == 0) {
                            if (remove(IP_FILE) == -1) {
                                ERROR("Error when remove file");
                            }
                        }
#ifdef CLIENT_MODE
                        return 0;
#endif
                    } else {
                        if (just_connect && strstr(data_msg_buf, "\r\n")) {
                            int width = 800;
                            int height = 480;
                            PRINT("Receive control data(%u): %s\n", len, data_msg_buf);
                            char *control_end = strstr(data_msg_buf, "\r\n\r\n");
                            int bdata_len = 0;
                            if (control_end + 4 - data_msg_buf > 0) {
                                bdata_len = len - (control_end + 4 - data_msg_buf);
                                control_end = control_end + 4;
                            }
                            //PRINT("bdata_len=%d\n", bdata_len);
                            char *info = strtok(data_msg_buf, "\r\n");
                            while (info) {
                                //PRINT("info: %s\n", info);
                                if (strstr(info, "X-WIDTH:")) {
                                    width = atoi(strstr(info, " "));
                                    PRINT("width: %d\n", width);
                                }
                                if (strstr(info, "X-HEIGHT:")) {
                                    height = atoi(strstr(info, " "));
                                    PRINT("height: %d\n", height);
                                }
                                info = strtok(NULL, "\r\n");
                            }

                            if (!strncmp(gst_sink, "ffplay", 6)) {
#if USE_FIFO
                                char * const command[] = {"ffplay", "-framerate", "50", "-infbuf", "-framedrop", "-analyzeduration", "1", FIFO_PATH, NULL};
#else
                                char * const command[] = {"ffplay", "-infbuf", "-framedrop", "-analyzeduration", "1", "-", NULL};
                                //const char *command[] = {"ffplay", "-", NULL};
#endif
                                gst_pid = popen2(command, &gst_in_fp, &gst_out_fp);
                            } else if (!strncmp(gst_sink, "ffvademo", 8)) {
                                //const char *command[] = {"ffvademo", "-x", "1920", "-y", "1080", "/dev/stdin", NULL};
                                char * const command[] = {"ffvademo", "/dev/stdin", NULL};
                                gst_pid = popen2(command, &gst_in_fp, &gst_out_fp);
                            } else if (!strncmp(gst_sink, "ffvadisplay", 11)) {
                                char * const command[] = {"ffvadisplay", "-x", "1920", "-y", "1200", "/dev/stdin", NULL};
                                gst_pid = popen2(command, &gst_in_fp, &gst_out_fp);
                            } else {
#if USE_FIFO
                                char location_buf[32] = {0};
                                strcat(location_buf, "location=");
                                strcat(location_buf, FIFO_PATH);
#ifdef VPUDEC
                                char mime_buf[70] = {0};
                                snprintf(mime_buf, 70, "video\/x-h264,width=%d,height=%d,framerate=30\/1", width, height);
                                //snprintf(mime_buf, 70, "video\/x-h264,width=%d,height=%d,framerate=30\/1,stream-format=avc", width, height);
                                PRINT("Using cap: %s\n", mime_buf);
                                char * const command[] = {"gst-launch-0.10", "filesrc", location_buf, "do-timestamp=true", "!", mime_buf, "!", "vpudec", "framedrop=true", "frame-plus=1", "low-latency=true", "!", gst_sink, NULL};
#else
                                char * const command[] = {"gst-launch-1.0", "filesrc", location_buf, "do-timestamp=true", "!", "h264parse", "!", "decodebin", "!", gst_sink, NULL};
#endif
#else
#ifdef VPUDEC
                                char mime_buf[70] = {0};
                                snprintf(mime_buf, 70, "video\/x-h264,width=%d,height=%d,framerate=30\/1", width, height);
                                //snprintf(mime_buf, 70, "video\/x-h264,width=%d,height=%d,framerate=30\/1,stream-format=avc", width, height);
                                PRINT("Using cap: %s\n", mime_buf);
                                char * const command[] = {"gst-launch-0.10", "fdsrc", "do-timestamp=true", "!", mime_buf, "!", "vpudec", "framedrop=false", "frame-plus=1", "low-latency=true", "!", gst_sink, NULL};
#else
                                char * const command[] = {"gst-launch-1.0", "fdsrc", "do-timestamp=true", "!", "h264parse", "!", "decodebin", "!" , "videorate", "!", gst_sink, NULL};
#endif
#endif
                                PRINT("command:%s\n", *command);
                                gst_pid = popen2(command, &gst_in_fp, &gst_out_fp);
                            }
                            PRINT("gst pid: %d\n", gst_pid);
                            PRINT("gst in fp: %d\n", gst_in_fp);
#if USE_FIFO
                            fifo_fp = open(FIFO_PATH, O_WRONLY);
                            PRINT("fifo_fp: %d\n", fifo_fp);
#endif

                            just_connect = 0;
                            if (bdata_len > 0) {
#if USE_FIFO
                                if (fifo_fp > 0) {
                                    len = write(fifo_fp, control_end, bdata_len);
                                    PRINT("Write non control data len: %d\n", len);
                                }
#else
                                if (gst_in_fp > 0) {
                                    len = write(gst_in_fp, control_end, bdata_len);
                                    PRINT("Write non control data len: %d\n", len);
                                }
#endif
                            }
                        } else {
#if USE_FIFO
                            if (fifo_fp > 0) {
                                len = write(fifo_fp, data_msg_buf, len);
                                //PRINT("Write to fifo len: %d\n", len);
                                if (len < 0) {
                                    ERROR("Pipe input error");
                                }
                            }
#else
                            if (gst_in_fp > 0) {
                                len = write(gst_in_fp, data_msg_buf, len);
                                //PRINT("Piped len: %d\n", len);
                                if (len < 0) {
                                    ERROR("Pipe input error");
                                }
                            }
#endif
                        }
                    }
                } else {
                    if (timeout) {
                        if (gst_pid > 0) {
                            no_data_count++;
                            // 3 * 10 = 30 seconds
                            if (no_data_count > 1) {
                                PRINT("No data for casting after 10 seconds, close the socket and receiver\n");
                                if (tcp_client_sock > 0) {
                                    close(tcp_client_sock);
                                    tcp_client_sock = -1;
                                }
                                if (gst_pid > 0) {
                                    kill(gst_pid, SIGKILL);
                                    waitpid(gst_pid, NULL, 0);
                                    gst_pid = -1;
                                    gst_in_fp = -1;
                                    gst_out_fp = -1;
                                }
                                if (fifo_fp > 0) {
                                    close(fifo_fp);
                                    fifo_fp = -1;
                                }
                                is_connected = 0;
                                if (access(IP_FILE, F_OK) == 0) {
                                    if (remove(IP_FILE) == -1) {
                                        ERROR("Error when remove file");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return 0;
}
