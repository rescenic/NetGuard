/*
    This file is part of NetGuard.

    NetGuard is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NetGuard is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NetGuard.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2015-2016 by Marcel Bokhorst (M66B)
*/

#include <jni.h>
#include <android/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>

#include "netguard.h"

// #define PROFILE 1

// TODO TCP options
// TODO TCP fragmentation
// TODO non blocking send/write/close, handle EAGAIN/EWOULDBLOCK

// It is assumed that no packets will get lost and that packets arrive in order

// Global variables

static JavaVM *jvm;
pthread_t thread_id;
pthread_mutex_t lock;
jboolean stopping = 0;
jboolean signaled = 0;

struct udp_session *udp_session = NULL;
struct tcp_session *tcp_session = NULL;

int debug = 0;
int loglevel = 0;
FILE *pcap_file = NULL;

// JNI

jclass clsPacket;

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    log_android(ANDROID_LOG_INFO, "JNI load");

    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        log_android(ANDROID_LOG_INFO, "JNI load GetEnv failed");
        return -1;
    }

    const char *packet = "eu/faircode/netguard/Packet";
    clsPacket = jniGlobalRef(env, jniFindClass(env, packet));

    return JNI_VERSION_1_6;
}

void JNI_OnUnload(JavaVM *vm, void *reserved) {
    log_android(ANDROID_LOG_INFO, "JNI unload");

    JNIEnv *env;
    if ((*vm)->GetEnv(vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK)
        log_android(ANDROID_LOG_INFO, "JNI load GetEnv failed");
    else {
        (*env)->DeleteGlobalRef(env, clsPacket);
    }
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1init(JNIEnv *env) {
    udp_session = NULL;
    tcp_session = NULL;
    loglevel = ANDROID_LOG_WARN;
    pcap_file = NULL;

    if (pthread_mutex_init(&lock, NULL))
        log_android(ANDROID_LOG_ERROR, "pthread_mutex_init failed");
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1start(
        JNIEnv *env, jobject instance,
        jint tun, jintArray uids_,
        jstring hosts_,
        jboolean log, jboolean filter,
        jboolean debug_, jint loglevel_) {

    debug = debug_;
    loglevel = loglevel_;

    log_android(ANDROID_LOG_WARN, "Starting tun=%d log %d filter %d debug %d level %d",
                tun, log, filter, debug, loglevel);

    // Set blocking
    int flags = fcntl(tun, F_GETFL, 0);
    if (flags < 0 || fcntl(tun, F_SETFL, flags & ~O_NONBLOCK) < 0)
        log_android(ANDROID_LOG_ERROR, "fcntl tun ~O_NONBLOCK error %d: %s",
                    errno, strerror(errno));

    if (pthread_kill(thread_id, 0) == 0)
        log_android(ANDROID_LOG_WARN, "Already running thread %lu", thread_id);
    else {
        jint rs = (*env)->GetJavaVM(env, &jvm);
        if (rs != JNI_OK)
            log_android(ANDROID_LOG_ERROR, "GetJavaVM failed");

        // Get arguments
        struct arguments *args = malloc(sizeof(struct arguments));
        // args->env = will be set in thread
        args->instance = (*env)->NewGlobalRef(env, instance);
        args->tun = tun;

        args->ucount = (*env)->GetArrayLength(env, uids_);
        args->uids = malloc(args->ucount * sizeof(jint));
        jint *uids = (*env)->GetIntArrayElements(env, uids_, NULL);
        memcpy(args->uids, uids, args->ucount * sizeof(jint));
        (*env)->ReleaseIntArrayElements(env, uids_, uids, 0);

        args->log = log;
        args->filter = filter;

        if (hosts_ == NULL) {
            args->hcount = 0;
            args->hosts = NULL;
            log_android(ANDROID_LOG_WARN, "No hosts file");
        } else {
            const char *hosts = (*env)->GetStringUTFChars(env, hosts_, 0);
            log_android(ANDROID_LOG_WARN, "hosts file %s", hosts);
            read_hosts(hosts, args);
            (*env)->ReleaseStringUTFChars(env, hosts_, hosts);
        }

        for (int i = 0; i < args->ucount; i++)
            log_android(ANDROID_LOG_VERBOSE, "Allowed uid %d", args->uids[i]);

        // Terminate sessions not allowed anymore
        check_allowed(args);

        // Start native thread
        int err = pthread_create(&thread_id, NULL, handle_events, (void *) args);
        if (err == 0)
            log_android(ANDROID_LOG_INFO, "Started thread %lu", thread_id);
        else
            log_android(ANDROID_LOG_ERROR, "pthread_create error %d: %s", err, strerror(err));
    }
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1stop(JNIEnv *env, jobject instance,
                                                    jint tun, jboolean clear) {
    log_android(ANDROID_LOG_WARN, "Stop tun %d clear %d", tun, (int) clear);
    if (pthread_kill(thread_id, 0) == 0) {
        stopping = 1;
        log_android(ANDROID_LOG_DEBUG, "Kill thread %lu", thread_id);
        int err = pthread_kill(thread_id, SIGUSR1);
        if (err != 0)
            log_android(ANDROID_LOG_WARN, "pthread_kill error %d: %s", err, strerror(err));
        else {
            log_android(ANDROID_LOG_DEBUG, "Join thread %lu", thread_id);
            pthread_join(thread_id, NULL);
            if (err != 0)
                log_android(ANDROID_LOG_WARN, "pthread_join error %d: %s", err, strerror(err));
        }

        if (clear)
            clear_sessions();

        log_android(ANDROID_LOG_INFO, "Stopped thread %lu", thread_id);
    } else
        log_android(ANDROID_LOG_WARN, "Not running");
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1done(JNIEnv *env, jobject instance) {
    log_android(ANDROID_LOG_INFO, "Done");

    clear_sessions();

    if (pthread_mutex_destroy(&lock))
        log_android(ANDROID_LOG_ERROR, "pthread_mutex_destroy failed");
}

JNIEXPORT void JNICALL
Java_eu_faircode_netguard_SinkholeService_jni_1pcap(JNIEnv *env, jclass type,
                                                    jstring name_, jboolean init) {
    if (pthread_mutex_lock(&lock))
        log_android(ANDROID_LOG_ERROR, "pthread_mutex_lock failed");

    if (name_ == NULL) {
        if (pcap_file != NULL) {
            int flags = fcntl(fileno(pcap_file), F_GETFL, 0);
            if (flags < 0 || fcntl(fileno(pcap_file), F_SETFL, flags & ~O_NONBLOCK) < 0)
                log_android(ANDROID_LOG_ERROR, "PCAP fcntl ~O_NONBLOCK error %d: %s",
                            errno, strerror(errno));

            if (fsync(fileno(pcap_file)))
                log_android(ANDROID_LOG_ERROR, "PCAP fsync error %d: %s", errno, strerror(errno));

            if (fclose(pcap_file))
                log_android(ANDROID_LOG_ERROR, "PCAP fclose error %d: %s", errno, strerror(errno));

            pcap_file = NULL;
        }
        log_android(ANDROID_LOG_INFO, "PCAP disabled");
    }
    else {
        const char *name = (*env)->GetStringUTFChars(env, name_, 0);
        log_android(ANDROID_LOG_INFO, "PCAP file %s", name);

        pcap_file = fopen(name, "ab+");
        if (pcap_file == NULL)
            log_android(ANDROID_LOG_ERROR, "PCAP fopen error %d: %s", errno, strerror(errno));
        else {
            int flags = fcntl(fileno(pcap_file), F_GETFL, 0);
            if (flags < 0 || fcntl(fileno(pcap_file), F_SETFL, flags | O_NONBLOCK) < 0)
                log_android(ANDROID_LOG_ERROR, "PCAP fcntl O_NONBLOCK error %d: %s",
                            errno, strerror(errno));

            if (init)
                write_pcap_hdr();
        }

        (*env)->ReleaseStringUTFChars(env, name_, name);
    }

    if (pthread_mutex_unlock(&lock))
        log_android(ANDROID_LOG_ERROR, "pthread_mutex_unlock failed");
}

JNIEXPORT jstring JNICALL
Java_eu_faircode_netguard_Util_jni_1getprop(JNIEnv *env, jclass type, jstring name_) {
    const char *name = (*env)->GetStringUTFChars(env, name_, 0);

    char value[250];
    __system_property_get(name, value);

    (*env)->ReleaseStringUTFChars(env, name_, name);

    return (*env)->NewStringUTF(env, value);
}

// Private functions

void check_allowed(const struct arguments *args) {
    struct udp_session *u = udp_session;
    while (u != NULL) {
        if (!u->stop) {
            int found = 0;
            for (int i = 0; i < args->ucount; i++)
                if (u->uid == args->uids[i]) {
                    found = 1;
                    break;
                }
            if (!found) {
                u->stop = 1;
                log_android(ANDROID_LOG_WARN, "UDP terminate %d uid %d", u->socket, u->uid);
            }
        }
        u = u->next;
    }

    struct tcp_session *t = tcp_session;
    while (t != NULL) {
        if (t->state != TCP_TIME_WAIT && t->state != TCP_CLOSE) {
            int found = 0;
            for (int i = 0; i < args->ucount; i++)
                if (t->uid == args->uids[i]) {
                    found = 1;
                    break;
                }
            if (!found) {
                t->state = TCP_TIME_WAIT;
                log_android(ANDROID_LOG_WARN, "TCP terminate socket %d uid %d", t->socket, t->uid);
            }
        }
        t = t->next;
    }
}

void clear_sessions() {
    struct udp_session *u = udp_session;
    while (u != NULL) {
        if (u->socket >= 0 && close(u->socket))
            log_android(ANDROID_LOG_ERROR, "UDP close %d error %d: %s",
                        u->socket, errno, strerror(errno));
        struct udp_session *p = u;
        u = u->next;
        free(p);
    }
    udp_session = NULL;

    struct tcp_session *t = tcp_session;
    while (t != NULL) {
        if (t->socket >= 0 && close(t->socket))
            log_android(ANDROID_LOG_ERROR, "TCP close %d error %d: %s",
                        u->socket, errno, strerror(errno));
        struct tcp_session *p = t;
        t = t->next;
        free(p);
    }
    tcp_session = NULL;
}

void handle_signal(int sig, siginfo_t *info, void *context) {
    log_android(ANDROID_LOG_DEBUG, "Signal %d", sig);
    signaled = 1;
}

void *handle_events(void *a) {
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    struct timespec ts;
    sigset_t blockset;
    sigset_t emptyset;
    struct sigaction sa;

    struct arguments *args = (struct arguments *) a;
    log_android(ANDROID_LOG_INFO, "Start events tun=%d thread %lu", args->tun, thread_id);

    // Attach to Java
    JNIEnv *env;
    jint rs = (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    if (rs != JNI_OK) {
        log_android(ANDROID_LOG_ERROR, "AttachCurrentThread failed");
        return NULL;
    }
    args->env = env;

    // Block SIGUSR1
    sigemptyset(&blockset);
    sigaddset(&blockset, SIGUSR1);
    sigprocmask(SIG_BLOCK, &blockset, NULL);

    /// Handle SIGUSR1
    sa.sa_sigaction = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    stopping = 0;
    signaled = 0;

    // Loop
    while (1) {
        log_android(ANDROID_LOG_DEBUG, "Loop thread %lu", thread_id);

        // Check sessions
        check_sessions(args);

        // Select
        ts.tv_sec = SELECT_TIMEOUT;
        ts.tv_nsec = 0;
        sigemptyset(&emptyset);
        int max = get_selects(args, &rfds, &wfds, &efds);
        int ready = pselect(max + 1, &rfds, &wfds, &efds,
                            udp_session == NULL && tcp_session == NULL ? NULL : &ts,
                            &emptyset);

        if (ready < 0) {
            if (errno == EINTR) {
                if (stopping && signaled) { ;
                    log_android(ANDROID_LOG_WARN, "pselect signaled tun %d", args->tun);
                    report_exit(args, NULL);
                    break;
                } else {
                    // TODO check if SIGUSR1 is free
                    log_android(ANDROID_LOG_DEBUG, "pselect interrupted %d", args->tun);
                    continue;
                }
            } else {
                log_android(ANDROID_LOG_ERROR, "pselect tun %d error %d: %s",
                            args->tun, errno, strerror(errno));
                report_exit(args, "pselect tun %d error %d: %s",
                            args->tun, errno, strerror(errno));
                break;
            }
        }

        // Count sessions
        int udp = 0;
        struct udp_session *u = udp_session;
        while (u != NULL) {
            udp++;
            u = u->next;
        }

        int tcp = 0;
        struct tcp_session *t = tcp_session;
        while (t != NULL) {
            tcp++;
            t = t->next;
        }

        if (ready == 0)
            log_android(ANDROID_LOG_DEBUG, "pselect timeout udp %d tcp %d", udp, tcp);
        else {
            log_android(ANDROID_LOG_DEBUG, "pselect udp %d tcp %d ready %d", udp, tcp, ready);

#ifdef PROFILE
            struct timeval start, end;
            float mselapsed;
            gettimeofday(&start, NULL);
#endif

            if (pthread_mutex_lock(&lock))
                log_android(ANDROID_LOG_ERROR, "pthread_mutex_lock failed");

            // Check upstream
            int error;
            if (check_tun(args, &rfds, &wfds, &efds) < 0)
                error = 1;
            else {
#ifdef PROFILE
                gettimeofday(&end, NULL);
                mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                            (end.tv_usec - start.tv_usec) / 1000.0;
                if (mselapsed > 1)
                    log_android(ANDROID_LOG_INFO, "tun %f", mselapsed);

                gettimeofday(&start, NULL);
#endif

                // Check UDP downstream
                check_udp_sockets(args, &rfds, &wfds, &efds);

                // Check TCP downstream
                check_tcp_sockets(args, &rfds, &wfds, &efds);
            }

            if (pthread_mutex_unlock(&lock))
                log_android(ANDROID_LOG_ERROR, "pthread_mutex_unlock failed");

            if (error)
                break;

#ifdef PROFILE
            gettimeofday(&end, NULL);
            mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                        (end.tv_usec - start.tv_usec) / 1000.0;
            if (mselapsed > 1)
                log_android(ANDROID_LOG_INFO, "sockets %f", mselapsed);
#endif
        }
    }

    (*env)->DeleteGlobalRef(env, args->instance);

    // Detach from Java
    rs = (*jvm)->DetachCurrentThread(jvm);
    if (rs != JNI_OK)
        log_android(ANDROID_LOG_ERROR, "DetachCurrentThread failed");

    // Cleanup
    free(args->uids);
    for (int i = 0; i < args->hcount; i++)
        free(args->hosts[i]);
    free(args->hosts);
    free(args);

    log_android(ANDROID_LOG_WARN, "Stopped events tun=%d thread %lu", args->tun, thread_id);
    return NULL;
}

void report_exit(const struct arguments *args, const char *fmt, ...) {
    jclass cls = (*args->env)->GetObjectClass(args->env, args->instance);
    jmethodID mid = jniGetMethodID(args->env, cls, "nativeExit", "(Ljava/lang/String;)V");

    jstring jreason = NULL;
    if (fmt != NULL) {
        char line[1024];
        va_list argptr;
        va_start(argptr, fmt);
        vsprintf(line, fmt, argptr);
        jreason = (*args->env)->NewStringUTF(args->env, line);
        va_end(argptr);
    }

    (*args->env)->CallVoidMethod(args->env, args->instance, mid, jreason);
    jniCheckException(args->env);

    if (jreason != NULL)
        (*args->env)->DeleteLocalRef(args->env, jreason);
    (*args->env)->DeleteLocalRef(args->env, cls);
}

void check_sessions(const struct arguments *args) {
    time_t now = time(NULL);

    // Check UDP sessions
    struct udp_session *ul = NULL;
    struct udp_session *u = udp_session;
    while (u != NULL) {
        int timeout;
        if (ntohs(u->dest) == 53)
            timeout = UDP_TIMEOUT_53;
        else
            timeout = UDP_TIMEOUT_ANY;
        if (u->stop || u->time + timeout < now) {
            char source[INET6_ADDRSTRLEN + 1];
            char dest[INET6_ADDRSTRLEN + 1];
            if (u->version == 4) {
                inet_ntop(AF_INET, &u->saddr.ip4, source, sizeof(source));
                inet_ntop(AF_INET, &u->daddr.ip4, dest, sizeof(dest));
            }
            else {
                inet_ntop(AF_INET6, &u->saddr.ip6, source, sizeof(source));
                inet_ntop(AF_INET6, &u->daddr.ip6, dest, sizeof(dest));
            }
            log_android(ANDROID_LOG_WARN, "UDP idle %d/%d sec stop %d from %s/%u to %s/%u",
                        now - u->time, timeout, u->stop,
                        dest, ntohs(u->dest), source, ntohs(u->source));

            if (close(u->socket))
                log_android(ANDROID_LOG_ERROR, "UDP close %d error %d: %s",
                            u->socket, errno, strerror(errno));
            u->socket = -1;

            if (ul == NULL)
                udp_session = u->next;
            else
                ul->next = u->next;

            struct udp_session *c = u;
            u = u->next;
            free(c);
        }
        else {
            ul = u;
            u = u->next;
        }
    }

    // Check TCP sessions
    struct tcp_session *tl = NULL;
    struct tcp_session *t = tcp_session;
    while (t != NULL) {
        char source[INET6_ADDRSTRLEN + 1];
        char dest[INET6_ADDRSTRLEN + 1];
        if (t->version == 4) {
            inet_ntop(AF_INET, &t->saddr.ip4, source, sizeof(source));
            inet_ntop(AF_INET, &t->daddr.ip4, dest, sizeof(dest));
        } else {
            inet_ntop(AF_INET6, &t->saddr.ip6, source, sizeof(source));
            inet_ntop(AF_INET6, &t->daddr.ip6, dest, sizeof(dest));
        }

        // Check connection timeout
        int timeout = 0;
        if (t->state == TCP_LISTEN || t->state == TCP_SYN_RECV)
            timeout = TCP_INIT_TIMEOUT;
        else if (t->state == TCP_ESTABLISHED)
            timeout = TCP_IDLE_TIMEOUT;
        else
            timeout = TCP_CLOSE_TIMEOUT;
        if (t->state != TCP_TIME_WAIT && t->state != TCP_CLOSE && t->time + timeout < now) {
            // TODO send keep alives?
            log_android(ANDROID_LOG_WARN, "Idle %d/%d sec from %s/%u to %s/%u state %s",
                        now - t->time, timeout,
                        source, ntohs(t->source), dest, ntohs(t->dest), strstate(t->state));

            write_rst(args, t);
        }

        // Check finished connection
        if (t->state == TCP_TIME_WAIT) {
            log_android(ANDROID_LOG_INFO, "Close from %s/%u to %s/%u socket %d",
                        source, ntohs(t->source), dest, ntohs(t->dest), t->socket);

            if (close(t->socket))
                log_android(ANDROID_LOG_ERROR, "close %d error %d: %s",
                            t->socket, errno, strerror(errno));
            t->socket = -1;

            t->time = time(NULL);
            t->state = TCP_CLOSE;
        }

        // Cleanup lingering sessions
        if (t->state == TCP_CLOSE && t->time + TCP_KEEP_TIMEOUT < now) {
            if (tl == NULL)
                tcp_session = t->next;
            else
                tl->next = t->next;

            struct tcp_session *c = t;
            t = t->next;
            free(c);
        }
        else {
            tl = t;
            t = t->next;
        }
    }
}

int get_selects(const struct arguments *args, fd_set *rfds, fd_set *wfds, fd_set *efds) {
    // Initialize
    FD_ZERO(rfds);
    FD_ZERO(wfds);
    FD_ZERO(efds);

    // Always select tun
    FD_SET(args->tun, rfds);
    FD_SET(args->tun, efds);
    int max = args->tun;

    // Select UDP sockets
    struct udp_session *u = udp_session;
    while (u != NULL) {
        if (!u->stop) {
            FD_SET(u->socket, efds);
            FD_SET(u->socket, rfds);
            if (u->socket > max)
                max = u->socket;
        }
        u = u->next;
    }

    // Select TCP sockets
    struct tcp_session *t = tcp_session;
    while (t != NULL) {
        // Select sockets
        if (t->state == TCP_LISTEN) {
            // Check for connected / errors
            FD_SET(t->socket, efds);
            FD_SET(t->socket, wfds);
            if (t->socket > max)
                max = t->socket;
        }
        else if (t->state == TCP_ESTABLISHED ||
                 t->state == TCP_SYN_RECV ||
                 t->state == TCP_CLOSE_WAIT) {
            // Check for data / errors
            FD_SET(t->socket, efds);
            if (t->send_window > 0)
                FD_SET(t->socket, rfds);
            if (t->socket > max)
                max = t->socket;
        }

        t = t->next;
    }

    return max;
}

int check_tun(const struct arguments *args, fd_set *rfds, fd_set *wfds, fd_set *efds) {
    // Check tun error
    if (FD_ISSET(args->tun, efds)) {
        log_android(ANDROID_LOG_ERROR, "tun %d exception", args->tun);
        if (fcntl(args->tun, F_GETFL) < 0) {
            log_android(ANDROID_LOG_ERROR, "fcntl tun %d F_GETFL error %d: %s",
                        args->tun, errno, strerror(errno));
            report_exit(args, "fcntl tun %d F_GETFL error %d: %s",
                        args->tun, errno, strerror(errno));
        } else
            report_exit(args, "tun %d exception", args->tun);
        return -1;
    }

    // Check tun read
    if (FD_ISSET(args->tun, rfds)) {
        uint8_t buffer[TUN_MAXMSG];
        ssize_t length = read(args->tun, buffer, sizeof(buffer));
        if (length < 0) {
            log_android(ANDROID_LOG_ERROR, "tun read error %d: %s", errno, strerror(errno));
            if (errno == EINTR)
                return 0;
            else {
                report_exit(args, "tun read error %d: %s", errno, strerror(errno));
                return -1;
            }
        }
        else if (length > 0) {
            // Write pcap record
            if (pcap_file != NULL)
                write_pcap_rec(buffer, (size_t) length);

            // Handle IP from tun
            handle_ip(args, buffer, (size_t) length);
        }
        else {
            // tun eof
            log_android(ANDROID_LOG_ERROR, "tun %d empty read", args->tun);
            report_exit(args, "tun %d empty read", args->tun);
            return -1;
        }
    }

    return 0;
}

void check_udp_sockets(const struct arguments *args, fd_set *rfds, fd_set *wfds, fd_set *efds) {
    struct udp_session *cur = udp_session;
    while (cur != NULL) {
        if (cur->socket >= 0) {
            // Check socket error
            if (FD_ISSET(cur->socket, efds)) {
                cur->time = time(NULL);

                int serr = 0;
                socklen_t optlen = sizeof(int);
                int err = getsockopt(cur->socket, SOL_SOCKET, SO_ERROR, &serr, &optlen);
                if (err < 0)
                    log_android(ANDROID_LOG_ERROR, "UDP getsockopt error %d: %s",
                                errno, strerror(errno));
                else if (serr)
                    log_android(ANDROID_LOG_ERROR, "UDP SO_ERROR %d: %s", serr, strerror(serr));

                cur->stop = 1;
            }
            else {
                // Check socket read
                if (FD_ISSET(cur->socket, rfds)) {
                    cur->time = time(NULL);

                    uint8_t buffer[UDP4_MAXMSG];
                    ssize_t bytes = recv(cur->socket, buffer, sizeof(buffer), 0);
                    if (bytes < 0) {
                        // Socket error
                        log_android(ANDROID_LOG_ERROR, "UDP recv error %d: %s",
                                    errno, strerror(errno));

                        if (errno != EINTR)
                            cur->stop = 1;
                    }
                    else if (bytes == 0) {
                        // Socket eof
                        log_android(ANDROID_LOG_WARN, "UDP recv empty");
                        cur->stop = 1;

                    } else {
                        // Socket read data
                        char dest[INET6_ADDRSTRLEN + 1];
                        if (cur->version == 4)
                            inet_ntop(AF_INET, &cur->daddr.ip4, dest, sizeof(dest));
                        else
                            inet_ntop(AF_INET6, &cur->daddr.ip6, dest, sizeof(dest));
                        log_android(ANDROID_LOG_INFO, "UDP recv bytes %d from %s/%u @tun",
                                    bytes, dest, ntohs(cur->dest));
                        if (write_udp(args, cur, buffer, (size_t) bytes) < 0)
                            log_android(ANDROID_LOG_ERROR, "write UDP error %d: %s",
                                        errno, strerror((errno)));
                        else {
                            // Prevent too many open files
                            if (ntohs(cur->dest) == 53)
                                cur->stop = 1;
                        }
                    }
                }
            }
        }
        cur = cur->next;
    }
}

void check_tcp_sockets(const struct arguments *args, fd_set *rfds, fd_set *wfds, fd_set *efds) {
    struct tcp_session *cur = tcp_session;
    while (cur != NULL) {
        if (cur->socket >= 1) {
            int oldstate = cur->state;
            uint32_t oldlocal = cur->local_seq;
            uint32_t oldremote = cur->remote_seq;

            // TODO if logging
            char source[INET6_ADDRSTRLEN + 1];
            char dest[INET6_ADDRSTRLEN + 1];
            if (cur->version == 4) {
                inet_ntop(AF_INET, &cur->saddr.ip4, source, sizeof(source));
                inet_ntop(AF_INET, &cur->daddr.ip4, dest, sizeof(dest));
            } else {
                inet_ntop(AF_INET6, &cur->saddr.ip6, source, sizeof(source));
                inet_ntop(AF_INET6, &cur->daddr.ip6, dest, sizeof(dest));
            }

            // Check socket error
            if (FD_ISSET(cur->socket, efds)) {
                cur->time = time(NULL);

                int serr = 0;
                socklen_t optlen = sizeof(int);
                int err = getsockopt(cur->socket, SOL_SOCKET, SO_ERROR, &serr, &optlen);
                if (err < 0)
                    log_android(ANDROID_LOG_ERROR, "getsockopt error %d: %s", errno,
                                strerror(errno));
                else if (serr)
                    log_android(ANDROID_LOG_ERROR, "SO_ERROR %d: %s", serr, strerror(serr));

                write_rst(args, cur);
            }
            else {
                // Assume socket okay
                if (cur->state == TCP_LISTEN) {
                    // Check socket connect
                    if (FD_ISSET(cur->socket, wfds)) {
                        cur->time = time(NULL);

                        log_android(ANDROID_LOG_INFO, "Connected from %s/%u to %s/%u",
                                    source, ntohs(cur->source), dest, ntohs(cur->dest));

                        if (write_syn_ack(args, cur) >= 0) {
                            cur->local_seq++; // local SYN
                            cur->remote_seq++; // remote SYN
                            cur->state = TCP_SYN_RECV;
                        }
                    }
                }

                else if (cur->state == TCP_SYN_RECV ||
                         cur->state == TCP_ESTABLISHED ||
                         cur->state == TCP_CLOSE_WAIT) {
                    // Check socket read
                    if (FD_ISSET(cur->socket, rfds) && cur->send_window > 0) {
                        cur->time = time(NULL);

                        size_t len = (cur->send_window > TCP_SEND_WINDOW
                                      ? TCP_SEND_WINDOW
                                      : cur->send_window);
                        uint8_t *buffer = malloc(len);
                        ssize_t bytes = recv(cur->socket, buffer, len, 0);
                        if (bytes < 0) {
                            // Socket error
                            log_android(ANDROID_LOG_ERROR, "recv error %d: %s",
                                        errno, strerror(errno));

                            if (errno != EINTR)
                                write_rst(args, cur);
                        }
                        else if (bytes == 0) {
                            // Socket eof
                            // TCP: application close
                            log_android(ANDROID_LOG_INFO, "recv empty state %s",
                                        strstate(cur->state));

                            if (write_fin_ack(args, cur, 0) >= 0) {
                                cur->local_seq++; // local FIN

                                if (cur->state == TCP_SYN_RECV || cur->state == TCP_ESTABLISHED)
                                    cur->state = TCP_FIN_WAIT1;
                                else if (cur->state == TCP_CLOSE_WAIT)
                                    cur->state = TCP_LAST_ACK;
                                else
                                    log_android(ANDROID_LOG_ERROR, "Unknown state %s",
                                                strstate(cur->state));

                                log_android(ANDROID_LOG_INFO, "Half close state %s",
                                            strstate(cur->state));
                            }
                        } else {
                            // Socket read data
                            log_android(ANDROID_LOG_DEBUG,
                                        "recv bytes %d state %s", bytes, strstate(cur->state));

                            // Forward to tun
                            if (write_data(args, cur, buffer, (size_t) bytes) >= 0)
                                cur->local_seq += bytes;
                        }

                        free(buffer);
                    }
                }
            }

            if (cur->state != oldstate || cur->local_seq != oldlocal ||
                cur->remote_seq != oldremote)
                log_android(ANDROID_LOG_INFO,
                            "Session %s/%u new state %s local %u remote %u",
                            dest, ntohs(cur->dest), strstate(cur->state),
                            cur->local_seq - cur->local_start,
                            cur->remote_seq - cur->remote_start);
        }
        cur = cur->next;
    }
}

// https://en.wikipedia.org/wiki/IPv6_packet#Extension_headers
// http://www.iana.org/assignments/protocol-numbers/protocol-numbers.xhtml
int is_lower_layer(int protocol) {
    // No next header = 59
    return (protocol == 0 || // Hop-by-Hop Options
            protocol == 60 || // Destination Options (before routing header)
            protocol == 43 || // Routing
            protocol == 44 || // Fragment
            protocol == 51 || // Authentication Header (AH)
            protocol == 50 || // Encapsulating Security Payload (ESP)
            protocol == 60 || // Destination Options (before upper-layer header)
            protocol == 135); // Mobility
}

int is_upper_layer(int protocol) {
    return (protocol == IPPROTO_TCP ||
            protocol == IPPROTO_UDP ||
            protocol == IPPROTO_ICMP ||
            protocol == 58); // ICMP for IPv6
}

void handle_ip(const struct arguments *args, const uint8_t *pkt, const size_t length) {
    uint8_t protocol;
    void *saddr;
    void *daddr;
    char source[INET6_ADDRSTRLEN + 1];
    char dest[INET6_ADDRSTRLEN + 1];
    char flags[10];
    int flen = 0;
    uint8_t *payload;

#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif

    // Get protocol, addresses & payload
    uint8_t version = (*pkt) >> 4;
    if (version == 4) {
        struct iphdr *ip4hdr = (struct iphdr *) pkt;

        protocol = ip4hdr->protocol;
        saddr = &ip4hdr->saddr;
        daddr = &ip4hdr->daddr;

        if (ip4hdr->frag_off & IP_MF) {
            log_android(ANDROID_LOG_ERROR, "IP fragment");
            flags[flen++] = '+';
        }

        uint8_t ipoptlen = (uint8_t) ((ip4hdr->ihl - 5) * 4);
        payload = (uint8_t *) (pkt + sizeof(struct iphdr) + ipoptlen);

        if (ntohs(ip4hdr->tot_len) != length) {
            log_android(ANDROID_LOG_ERROR, "Invalid length %u header length %u",
                        length, ntohs(ip4hdr->tot_len));
            return;
        }

        if (loglevel < ANDROID_LOG_WARN) {
            if (!calc_checksum(0, (uint8_t *) ip4hdr, sizeof(struct iphdr))) {
                log_android(ANDROID_LOG_ERROR, "Invalid IP checksum");
                return;
            }
        }
    }
    else if (version == 6) {
        struct ip6_hdr *ip6hdr = (struct ip6_hdr *) pkt;

        // Skip extension headers
        uint16_t off = 0;
        protocol = ip6hdr->ip6_nxt;
        if (!is_upper_layer(protocol)) {
            log_android(ANDROID_LOG_WARN, "IP6 extension %d", protocol);
            off = sizeof(struct ip6_hdr);
            struct ip6_ext *ext = (struct ip6_ext *) (pkt + off);
            while (is_lower_layer(ext->ip6e_nxt) && !is_upper_layer(protocol)) {
                protocol = ext->ip6e_nxt;
                log_android(ANDROID_LOG_WARN, "IP6 extension %d", protocol);

                off += (8 + ext->ip6e_len);
                ext = (struct ip6_ext *) (pkt + off);
            }
            if (!is_upper_layer(protocol)) {
                off = 0;
                protocol = ip6hdr->ip6_nxt;
                log_android(ANDROID_LOG_WARN, "IP6 final extension %d", protocol);
            }
        }

        saddr = &ip6hdr->ip6_src;
        daddr = &ip6hdr->ip6_dst;

        payload = (uint8_t *) (pkt + sizeof(struct ip6_hdr) + off);

        // TODO check length
        // TODO checksum
    }
    else {
        log_android(ANDROID_LOG_WARN, "Unknown version %d", version);
        return;
    }

    inet_ntop(version == 4 ? AF_INET : AF_INET6, saddr, source, sizeof(source));
    inet_ntop(version == 4 ? AF_INET : AF_INET6, daddr, dest, sizeof(dest));

    // Get ports & flags
    jboolean syn = 0;
    int32_t sport = -1;
    int32_t dport = -1;
    if (protocol == IPPROTO_TCP) {
        struct tcphdr *tcp = (struct tcphdr *) payload;

        sport = ntohs(tcp->source);
        dport = ntohs(tcp->dest);

        if (tcp->syn) {
            syn = 1;
            flags[flen++] = 'S';
        }
        if (tcp->ack)
            flags[flen++] = 'A';
        if (tcp->psh)
            flags[flen++] = 'P';
        if (tcp->fin)
            flags[flen++] = 'F';
        if (tcp->rst)
            flags[flen++] = 'R';

        // TODO checksum
    } else if (protocol == IPPROTO_UDP) {
        struct udphdr *udp = (struct udphdr *) payload;

        sport = ntohs(udp->source);
        dport = ntohs(udp->dest);

        // TODO checksum (IPv6)
    }
    flags[flen] = 0;

    // Get uid
    jint uid = -1;
    if ((protocol == IPPROTO_TCP && (!args->filter || syn)) || protocol == IPPROTO_UDP) {
        log_android(ANDROID_LOG_INFO, "get uid %s/%u syn %d", dest, dport, syn);
        int tries = 0;
        usleep(1000 * UID_DELAY);
        while (uid < 0 && tries++ < UID_MAXTRY) {
            // Check IPv6 table first
            int dump = (tries == UID_MAXTRY);
            if (version == 4) {
                int8_t saddr128[16];
                memset(saddr128, 0, 10);
                saddr128[10] = (uint8_t) 0xFF;
                saddr128[11] = (uint8_t) 0xFF;
                memcpy(saddr128 + 12, saddr, 4);
                uid = get_uid(protocol, 6, saddr128, (const uint16_t) sport, dump);
            }

            if (uid < 0)
                uid = get_uid(protocol, version, saddr, (const uint16_t) sport, dump);

            // Retry delay
            if (uid < 0 && tries < UID_MAXTRY) {
                log_android(ANDROID_LOG_WARN, "get uid %s/%u syn %d try %d",
                            dest, dport, syn, tries);
                usleep(1000 * UID_DELAYTRY);
            }
        }

        if (uid < 0)
            log_android(ANDROID_LOG_ERROR, "uid not found");
    }

    log_android(ANDROID_LOG_DEBUG,
                "Packet v%d %s/%u -> %s/%u proto %d flags %s uid %d",
                version, source, sport, dest, dport, protocol, flags, uid);

#ifdef PROFILE
    gettimeofday(&end, NULL);
    mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_usec - start.tv_usec) / 1000.0;
    if (mselapsed > 1)
        log_android(ANDROID_LOG_INFO, "handle ip %f", mselapsed);
#endif

    // Check if allowed
    jboolean allowed = (jboolean) !syn;
    if (syn && args->filter) {
        for (int i = 0; i < args->ucount; i++)
            if (uid == args->uids[i]) {
                allowed = 1;
                break;
            }
    }

    // Handle allowed traffic
    int log = 0;
    char extra[200];
    *extra = 0;
    if (allowed) {
        if (protocol == IPPROTO_UDP) {
            allowed = handle_udp(args, pkt, length, payload, uid, extra);
            log = (!allowed || dport != 53);
        } else if (protocol == IPPROTO_TCP) {
            allowed = handle_tcp(args, pkt, length, payload, uid, extra);
            log = (!allowed || syn);
        }
        else {
            allowed = 0;
            log = 1;
        }
    }

    // Log traffic
    if (args->log && (!args->filter || log))
        log_packet(args, version, protocol, flags, source, sport, dest, dport, extra, uid, allowed);
}

jboolean handle_udp(const struct arguments *args,
                    const uint8_t *pkt, size_t length,
                    const uint8_t *payload,
                    int uid, char *extra) {
    // Get headers
    const uint8_t version = (*pkt) >> 4;
    const struct iphdr *ip4 = (struct iphdr *) pkt;
    const struct ip6_hdr *ip6 = (struct ip6_hdr *) pkt;
    const struct udphdr *udphdr = (struct udphdr *) payload;
    const uint8_t *data = payload + sizeof(struct udphdr);
    const size_t datalen = length - (data - pkt);

    // Search session
    struct udp_session *last = NULL;
    struct udp_session *cur = udp_session;
    while (cur != NULL &&
           !(cur->source == udphdr->source && cur->dest == udphdr->dest &&
             (version == 4 ? cur->saddr.ip4 == ip4->saddr &&
                             cur->daddr.ip4 == ip4->daddr
                           : memcmp(&cur->saddr.ip6, &ip6->ip6_src, 16) == 0 &&
                             memcmp(&cur->daddr.ip6, &ip6->ip6_dst, 16) == 0))) {
        last = cur;
        cur = cur->next;
    }

    char source[INET6_ADDRSTRLEN + 1];
    char dest[INET6_ADDRSTRLEN + 1];
    if (version == 4) {
        inet_ntop(AF_INET, &ip4->saddr, source, sizeof(source));
        inet_ntop(AF_INET, &ip4->daddr, dest, sizeof(dest));
    } else {
        inet_ntop(AF_INET6, &ip6->ip6_src, source, sizeof(source));
        inet_ntop(AF_INET6, &ip6->ip6_dst, dest, sizeof(dest));
    }

    // Create new session if needed
    if (cur == NULL) {
        log_android(ANDROID_LOG_INFO, "UDP new session from %s/%u to %s/%u",
                    source, ntohs(udphdr->source), dest, ntohs(udphdr->dest));

        // Register session
        struct udp_session *u = malloc(sizeof(struct udp_session));
        u->time = time(NULL);
        u->uid = uid;
        u->version = version;

        if (version == 4) {
            u->saddr.ip4 = (__be32) ip4->saddr;
            u->daddr.ip4 = (__be32) ip4->daddr;
        } else {
            memcpy(&u->saddr.ip6, &ip6->ip6_src, 16);
            memcpy(&u->daddr.ip6, &ip6->ip6_dst, 16);
        }

        u->source = udphdr->source;
        u->dest = udphdr->dest;
        u->stop = 0;
        u->next = NULL;

        // Open UDP socket
        u->socket = open_udp_socket(args, u);
        if (u->socket < 0) {
            free(u);
            return 0;
        }

        log_android(ANDROID_LOG_DEBUG, "UDP socket %d", u->socket);

        if (last == NULL)
            udp_session = u;
        else
            last->next = u;

        cur = u;
    }

    // Check for DNS
    if (ntohs(udphdr->dest) == 53) {
        char qname[DNS_QNAME_MAX + 1];
        uint16_t qtype;
        uint16_t qclass;
        if (get_dns(args, cur, data, datalen, &qtype, &qclass, qname) >= 0) {
            log_android(ANDROID_LOG_INFO, "DNS type %d class %d name %s", qtype, qclass, qname);
            sprintf(extra, "qtype %d qname %s", qtype, qname);

            if (check_domain(args, cur, data, datalen, qclass, qtype, qname)) {
                cur->stop = 1;
                return 0;
            }
        }
    }

    // Check for DHCP (tethering)
    if (ntohs(udphdr->source) == 68 || ntohs(udphdr->dest) == 67) {
        if (check_dhcp(args, cur, data, datalen) >= 0)
            return 1;
    }

    log_android(ANDROID_LOG_INFO, "UDP forward from tun %s/%u to %s/%u data %d",
                source, ntohs(udphdr->source), dest, ntohs(udphdr->dest), datalen);

    cur->time = time(NULL);

    struct sockaddr_in server4;
    struct sockaddr_in6 server6;
    if (version == 4) {
        server4.sin_family = (__kernel_sa_family_t) AF_INET;
        server4.sin_addr.s_addr = (__be32) ip4->daddr;
        server4.sin_port = udphdr->dest;
    } else {
        server6.sin6_family = (__kernel_sa_family_t) AF_INET6;
        memcpy(&server6.sin6_addr, &ip6->ip6_dst, 16);
        server6.sin6_port = udphdr->dest;
    }

    if (sendto(cur->socket, data, (socklen_t) datalen, MSG_NOSIGNAL,
               (const struct sockaddr *) (version == 4 ? &server4 : &server6),
               (socklen_t) (version == 4 ? sizeof(server4) : sizeof(server6))) != datalen) {
        log_android(ANDROID_LOG_ERROR, "UDP sendto error %d: %s", errno, strerror(errno));
        cur->stop = 1;
        return 0;
    }

    return 1;
}

int get_dns(const struct arguments *args, const struct udp_session *u,
            const uint8_t *data, const size_t datalen,
            uint16_t *qtype, uint16_t *qclass, char *name) {
    if (datalen < sizeof(struct dns_header) + 1) {
        log_android(ANDROID_LOG_WARN, "DNS packet length %d", datalen);
        return -1;
    }

    // Check if standard DNS query
    // TODO multiple qnames
    const struct dns_header *dns = (struct dns_header *) data;
    if (dns->qr == 0 && dns->opcode == 0 && ntohs(dns->q_count) > 0) {
        uint8_t noff = 0;

        // http://tools.ietf.org/html/rfc1035
        uint8_t len;
        uint8_t comp = 0;
        size_t qdoff = sizeof(struct dns_header);
        do {
            comp++;
            len = *(data + qdoff);

            // TODO DNS compression
            if (len & 0xC0) {
                log_android(ANDROID_LOG_WARN, "DNS compression len %x", len);
                return -1;
            }

            if (len && qdoff + 1 + len <= datalen) {
                memcpy(name + noff, data + qdoff + 1, len);
                *(name + noff + len) = '.';
                noff += (len + 1);
                qdoff += (1 + len);
            }
        } while (len && comp < 10);
        qdoff++;

        if (noff > 0 && qdoff + 4 == datalen) {
            *(name + noff - 1) = 0;
            *qtype = ntohs(*((uint16_t *) (data + qdoff)));
            *qclass = ntohs(*((uint16_t *) (data + qdoff + 2)));
            return 0;
        }
        else
            log_android(ANDROID_LOG_WARN, "DNS packet invalid");
    }

    return -1;
}

int check_domain(const struct arguments *args, const struct udp_session *u,
                 const uint8_t *data, const size_t datalen,
                 uint16_t qclass, uint16_t qtype, const char *name) {
    if (qclass == DNS_QCLASS_IN && (qtype == DNS_QTYPE_A || qtype == DNS_QTYPE_AAAA)) {
        for (int i = 0; i < args->hcount; i++)
            if (!strcmp(name, args->hosts[i])) {
                log_android(ANDROID_LOG_WARN, "DNS type %d name %s blocked", qtype, name);

                // Build response
                size_t rlen = datalen + sizeof(struct dns_rr) + (qtype == DNS_QTYPE_A ? 4 : 16);
                uint8_t *response = malloc(rlen);

                // Copy header & query
                memcpy(response, data, datalen);

                // Modify copied header
                struct dns_header *rh = (struct dns_header *) response;
                rh->qr = 1;
                rh->aa = 0;
                rh->tc = 0;
                rh->rd = 0;
                rh->ra = 0;
                rh->z = 0;
                rh->ad = 0;
                rh->cd = 0;
                rh->rcode = 0;
                rh->ans_count = htons(1);
                rh->auth_count = 0;
                rh->add_count = 0;

                // Build answer
                struct dns_rr *answer = (struct dns_rr *) (response + datalen);
                answer->qname_ptr = htons(sizeof(struct dns_header) | 0xC000);
                answer->qtype = htons(qtype);
                answer->qclass = htons(qclass);
                answer->ttl = htonl(DNS_TTL);
                answer->rdlength = htons(qtype == DNS_QTYPE_A ? 4 : 16);

                // Add answer address
                uint8_t *addr = response + datalen + sizeof(struct dns_rr);
                if (qtype == DNS_QTYPE_A)
                    inet_pton(AF_INET, "127.0.0.1", addr);
                else
                    inet_pton(AF_INET6, "::1", addr);

                // Send response
                ssize_t res = write_udp(args, u, response, rlen);

                free(response);

                if (res < 0)
                    log_android(ANDROID_LOG_ERROR, "write UDP error %d: %s",
                                errno, strerror((errno)));
                else
                    return 1;
            }
    }

    return 0;
}

int check_dhcp(const struct arguments *args, const struct udp_session *u,
               const uint8_t *data, const size_t datalen) {

    // This is untested
    // Android routing of DHCP is eroneous

    log_android(ANDROID_LOG_WARN, "DHCP check");

    if (datalen < sizeof(struct dhcp_packet)) {
        log_android(ANDROID_LOG_WARN, "DHCP packet size %d", datalen);
        return -1;
    }

    const struct dhcp_packet *request = (struct dhcp_packet *) data;

    if (ntohl(request->option_format) != DHCP_OPTION_MAGIC_NUMBER) {
        log_android(ANDROID_LOG_WARN, "DHCP invalid magic %x", request->option_format);
        return -1;
    }

    if (request->htype != 1 || request->hlen != 6) {
        log_android(ANDROID_LOG_WARN, "DHCP unknown hardware htype %d hlen %d",
                    request->htype, request->hlen);
        return -1;
    }

    log_android(ANDROID_LOG_WARN, "DHCP opcode", request->opcode);

    // Discover: source 0.0.0.0:68 destination 255.255.255.255:67
    // Offer: source 10.1.10.1:67 destination 255.255.255.255:68
    // Request: source 0.0.0.0:68 destination 255.255.255.255:67
    // Ack: source: 10.1.10.1 destination: 255.255.255.255

    if (request->opcode == 1) { // Discover/request
        struct dhcp_packet *response = calloc(500, 1);

        // Hack
        inet_pton(AF_INET, "10.1.10.1", &u->saddr);

        /*
        Discover:
            DHCP option 53: DHCP Discover
            DHCP option 50: 192.168.1.100 requested
            DHCP option 55: Parameter Request List:
            Request Subnet Mask (1), Router (3), Domain Name (15), Domain Name Server (6)

        Request
            DHCP option 53: DHCP Request
            DHCP option 50: 192.168.1.100 requested
            DHCP option 54: 192.168.1.1 DHCP server.
        */

        memcpy(response, request, sizeof(struct dhcp_packet));
        response->opcode = (request->siaddr == 0 ? 2 /* Offer */ : /* Ack */ 4);
        response->secs = 0;
        response->flags = 0;
        memset(&response->ciaddr, 0, sizeof(response->ciaddr));
        inet_pton(AF_INET, "10.1.10.2", &response->yiaddr);
        inet_pton(AF_INET, "10.1.10.1", &response->siaddr);
        memset(&response->giaddr, 0, sizeof(response->giaddr));

        // https://tools.ietf.org/html/rfc2132
        uint8_t *options = response + sizeof(struct dhcp_packet);

        int idx = 0;
        *(options + idx++) = 53; // Message type
        *(options + idx++) = 1;
        *(options + idx++) = (request->siaddr == 0 ? 2 : 5);
        /*
             1     DHCPDISCOVER
             2     DHCPOFFER
             3     DHCPREQUEST
             4     DHCPDECLINE
             5     DHCPACK
             6     DHCPNAK
             7     DHCPRELEASE
             8     DHCPINFORM
         */

        *(options + idx++) = 1; // subnet mask
        *(options + idx++) = 4; // IP4 length
        inet_pton(AF_INET, "255.255.255.0", options + idx);
        idx += 4;

        *(options + idx++) = 3; // gateway
        *(options + idx++) = 4; // IP4 length
        inet_pton(AF_INET, "10.1.10.1", options + idx);
        idx += 4;

        *(options + idx++) = 51; // lease time
        *(options + idx++) = 4; // quad
        *((uint32_t *) (options + idx)) = 3600;
        idx += 4;

        *(options + idx++) = 54; // DHCP
        *(options + idx++) = 4; // IP4 length
        inet_pton(AF_INET, "10.1.10.1", options + idx);
        idx += 4;

        *(options + idx++) = 6; // DNS
        *(options + idx++) = 4; // IP4 length
        inet_pton(AF_INET, "8.8.8.8", options + idx);
        idx += 4;

        *(options + idx++) = 255; // End

        /*
            DHCP option 53: DHCP Offer
            DHCP option 1: 255.255.255.0 subnet mask
            DHCP option 3: 192.168.1.1 router
            DHCP option 51: 86400s (1 day) IP address lease time
            DHCP option 54: 192.168.1.1 DHCP server
            DHCP option 6: DNS servers 9.7.10.15
         */

        if (write_udp(args, u, response, 500) < 0)
            log_android(ANDROID_LOG_ERROR, "write DHCP error %d: %s",
                        errno, strerror((errno)));
        else

            free(response);
    }
}

jboolean handle_tcp(const struct arguments *args,
                    const uint8_t *pkt, size_t length,
                    const uint8_t *payload,
                    int uid, char *extra) {
#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif
    // Get headers
    const uint8_t version = (*pkt) >> 4;
    const struct iphdr *ip4 = (struct iphdr *) pkt;
    const struct ip6_hdr *ip6 = (struct ip6_hdr *) pkt;
    const struct tcphdr *tcphdr = (struct tcphdr *) payload;
    const uint8_t *data = payload + sizeof(struct tcphdr);
    const size_t datalen = length - (data - pkt);

    // Search session
    struct tcp_session *last = NULL;
    struct tcp_session *cur = tcp_session;
    while (cur != NULL &&
           !(cur->source == tcphdr->source && cur->dest == tcphdr->dest &&
             (version == 4 ? cur->saddr.ip4 == ip4->saddr &&
                             cur->daddr.ip4 == ip4->daddr
                           : memcmp(&cur->saddr.ip6, &ip6->ip6_src, 16) == 0 &&
                             memcmp(&cur->daddr.ip6, &ip6->ip6_dst, 16) == 0))) {
        last = cur;
        cur = cur->next;
    }

    char source[INET6_ADDRSTRLEN + 1];
    char dest[INET6_ADDRSTRLEN + 1];
    if (version == 4) {
        inet_ntop(AF_INET, &ip4->saddr, source, sizeof(source));
        inet_ntop(AF_INET, &ip4->daddr, dest, sizeof(dest));
    } else {
        inet_ntop(AF_INET6, &ip6->ip6_src, source, sizeof(source));
        inet_ntop(AF_INET6, &ip6->ip6_dst, dest, sizeof(dest));
    }

    log_android(ANDROID_LOG_DEBUG,
                "TCP received from %s/%u for %s/%u seq %u ack %u window %u data %d",
                source, ntohs(tcphdr->source),
                dest, ntohs(tcphdr->dest),
                ntohl(tcphdr->seq) - (cur == NULL ? 0 : cur->remote_start),
                ntohl(tcphdr->ack_seq) - (cur == NULL ? 0 : cur->local_start),
                ntohs(tcphdr->window), datalen);

    if (cur == NULL) {
        if (tcphdr->syn) {
            log_android(ANDROID_LOG_INFO, "TCP new session from %s/%u to %s/%u window %u uid %d",
                        source, ntohs(tcphdr->source),
                        dest, ntohs(tcphdr->dest),
                        ntohs(tcphdr->window), uid);

            // Register session
            struct tcp_session *syn = malloc(sizeof(struct tcp_session));
            syn->time = time(NULL);
            syn->uid = uid;
            syn->version = version;
            syn->send_window = ntohs(tcphdr->window);
            syn->remote_seq = ntohl(tcphdr->seq); // ISN remote
            syn->local_seq = (uint32_t) rand(); // ISN local
            syn->remote_start = syn->remote_seq;
            syn->local_start = syn->local_seq;

            if (version == 4) {
                syn->saddr.ip4 = (__be32) ip4->saddr;
                syn->daddr.ip4 = (__be32) ip4->daddr;
            } else {
                memcpy(&syn->saddr.ip6, &ip6->ip6_src, 16);
                memcpy(&syn->daddr.ip6, &ip6->ip6_dst, 16);
            }

            syn->source = tcphdr->source;
            syn->dest = tcphdr->dest;
            syn->state = TCP_LISTEN;
            syn->next = NULL;

            // TODO handle SYN data?
            if (datalen)
                log_android(ANDROID_LOG_WARN, "TCP SYN session from %s/%u to %s/%u data %u",
                            source, ntohs(tcphdr->source),
                            dest, ntohs(tcphdr->dest), datalen);

            // Open socket
            syn->socket = open_tcp_socket(args, syn);
            if (syn->socket < 0) {
                // Remote might retry
                free(syn);
                return 0;
            }

            log_android(ANDROID_LOG_DEBUG, "TCP socket %d lport %d",
                        syn->socket, get_local_port(syn->socket));

            if (last == NULL)
                tcp_session = syn;
            else
                last->next = syn;
        }
        else {
            log_android(ANDROID_LOG_WARN, "TCP unknown session from %s/%u to %s/%u uid %d",
                        source, ntohs(tcphdr->source),
                        dest, ntohs(tcphdr->dest), uid);

            struct tcp_session rst;
            memset(&rst, 0, sizeof(struct tcp_session));
            rst.version = 4;
            rst.local_seq = 0;
            rst.remote_seq = ntohl(tcphdr->seq);

            if (version == 4) {
                rst.saddr.ip4 = (__be32) ip4->saddr;
                rst.daddr.ip4 = (__be32) ip4->daddr;
            } else {
                memcpy(&rst.saddr.ip6, &ip6->ip6_src, 16);
                memcpy(&rst.daddr.ip6, &ip6->ip6_dst, 16);
            }

            rst.source = tcphdr->source;
            rst.dest = tcphdr->dest;
            write_rst(args, &rst);

            return 0;
        }

#ifdef PROFILE
        gettimeofday(&end, NULL);
        mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                    (end.tv_usec - start.tv_usec) / 1000.0;
        if (mselapsed > 1)
            log_android(ANDROID_LOG_INFO, "new session %f", mselapsed);
#endif
    }
    else {
        // Session found
        if (cur->state == TCP_CLOSE) {
            log_android(ANDROID_LOG_WARN,
                        "TCP closed session from %s/%u to %s/%u state %s local %u remote %u",
                        source, ntohs(tcphdr->source),
                        dest, ntohs(cur->dest), strstate(cur->state),
                        cur->local_seq - cur->local_start,
                        cur->remote_seq - cur->remote_start);
            write_rst(args, cur);
            return 0;
        }
        else {
            int oldstate = cur->state;
            uint32_t oldlocal = cur->local_seq;
            uint32_t oldremote = cur->remote_seq;

            log_android(ANDROID_LOG_DEBUG,
                        "TCP session from %s/%u to %s/%u state %s local %u remote %u window %u",
                        source, ntohs(tcphdr->source),
                        dest, ntohs(cur->dest), strstate(cur->state),
                        cur->local_seq - cur->local_start,
                        cur->remote_seq - cur->remote_start,
                        ntohs(tcphdr->window));

            cur->time = time(NULL);
            cur->send_window = ntohs(tcphdr->window);

            // Do not change order of conditions

            // Forward data to socket
            int ok = 1;
            if (ntohl(tcphdr->seq) == cur->remote_seq && datalen) {
                log_android(ANDROID_LOG_DEBUG, "send socket data %u", datalen);

                int more = (tcphdr->psh ? 0 : MSG_MORE);
                if (send(cur->socket, data, datalen, MSG_NOSIGNAL | more) < 0) {
                    log_android(ANDROID_LOG_ERROR, "send error %d: %s", errno, strerror(errno));
                    write_rst(args, cur);
                    return 0;
                }

                if (tcphdr->fin ||
                    cur->state == TCP_FIN_WAIT1 ||
                    cur->state == TCP_FIN_WAIT2 ||
                    cur->state == TCP_CLOSING)
                    cur->remote_seq += datalen; // FIN will send ACK or no ACK
                else {
                    if (write_ack(args, cur, datalen) >= 0)
                        cur->remote_seq += datalen;
                    else
                        ok = 0;
                }
            }

            if (ok) {
                if (tcphdr->rst) {
                    // No sequence check
                    log_android(ANDROID_LOG_INFO, "TCP received RST from %s/%u to %s/%u state %s",
                                source, ntohs(tcphdr->source), dest, ntohs(cur->dest),
                                strstate(cur->state));
                    cur->state = TCP_TIME_WAIT;
                    return 0;
                }
                else {
                    if (ntohl(tcphdr->ack_seq) == cur->local_seq &&
                        ntohl(tcphdr->seq) == cur->remote_seq) {

                        if (tcphdr->syn) {
                            log_android(ANDROID_LOG_WARN,
                                        "TCP repeated SYN from %s/%u to %s/%u state %s",
                                        source, ntohs(tcphdr->source), dest, ntohs(cur->dest),
                                        strstate(cur->state));
                            // The socket is likely not opened yet
                            // Note: perfect, ordered packet receive assumed

                        } else if (tcphdr->fin /* ACK */) {
                            // Shutdown socket for writing
                            if (shutdown(cur->socket, SHUT_WR)) {
                                log_android(ANDROID_LOG_ERROR, "shutdown WR error %d: %s",
                                            errno, strerror(errno));
                                // Data might be lost
                                write_rst(args, cur);
                                return 0;
                            }
                            else {
                                if (write_ack(args, cur, 1) >= 0) {
                                    cur->remote_seq += 1; // FIN
                                    if (cur->state == TCP_ESTABLISHED /* && !tcphdr->ack */)
                                        cur->state = TCP_CLOSE_WAIT;
                                    else if (cur->state == TCP_FIN_WAIT1 && tcphdr->ack)
                                        cur->state = TCP_TIME_WAIT;
                                    else if (cur->state == TCP_FIN_WAIT1 && !tcphdr->ack)
                                        cur->state = TCP_CLOSING;
                                    else if (cur->state == TCP_FIN_WAIT2 /* && !tcphdr->ack */)
                                        cur->state = TCP_TIME_WAIT;
                                    else {
                                        log_android(ANDROID_LOG_ERROR,
                                                    "TCP invalid FIN from %s/%u to %s/%u state %s ACK %d",
                                                    source, ntohs(tcphdr->source),
                                                    dest, ntohs(cur->dest),
                                                    strstate(cur->state), tcphdr->ack);
                                        return 0;
                                    }
                                }
                                else {
                                    write_rst(args, cur);
                                    return 0;
                                }
                            }
                        }

                        else if (tcphdr->ack) {
                            if (cur->state == TCP_SYN_RECV)
                                cur->state = TCP_ESTABLISHED;
                            else if (cur->state == TCP_ESTABLISHED) {
                                log_android(ANDROID_LOG_DEBUG,
                                            "TCP new ACK from %s/%u to %s/%u state %s data %u",
                                            source, ntohs(tcphdr->source), dest, ntohs(cur->dest),
                                            strstate(cur->state), datalen);
                            }
                            else if (cur->state == TCP_LAST_ACK) {
                                // socket has been shutdown already
                                cur->state = TCP_TIME_WAIT;
                            }
                            else if (cur->state == TCP_FIN_WAIT1)
                                cur->state = TCP_FIN_WAIT2;
                            else if (cur->state == TCP_CLOSING)
                                cur->state = TCP_TIME_WAIT;
                            else {
                                log_android(ANDROID_LOG_ERROR,
                                            "TCP invalid ACK from %s/%u to %s/%u state %s",
                                            source, ntohs(tcphdr->source), dest, ntohs(cur->dest),
                                            strstate(cur->state));
                                return 0;
                            }
                        }

                        else {
                            log_android(ANDROID_LOG_ERROR,
                                        "TCP unknown packet from %s/%u to %s/%u state %s",
                                        source, ntohs(tcphdr->source), dest, ntohs(cur->dest),
                                        strstate(cur->state));
                            return 0;
                        }
                    }
                    else {
                        char *msg;
                        static char previous[] = "Previous";
                        static char repeated[] = "Repeated";
                        static char invalid[] = "Invalid";
                        static char keepalive[] = "Keep alive";

                        // TODO proper wrap around
                        jboolean allowed = 1;
                        if (tcphdr->ack && ((uint32_t) ntohl(tcphdr->seq) + 1) == cur->remote_seq)
                            msg = keepalive;
                        else if (ntohl(tcphdr->seq) == cur->remote_seq &&
                                 ntohl(tcphdr->ack_seq) < cur->local_seq)
                            msg = previous;
                        else if (ntohl(tcphdr->seq) < cur->remote_seq &&
                                 ntohl(tcphdr->ack_seq) == cur->local_seq)
                            msg = repeated;
                        else {
                            msg = invalid;
                            allowed = 0;
                        }

                        char flags[10];
                        int flen = 0;
                        if (tcphdr->syn)
                            flags[flen++] = 'S';
                        if (tcphdr->ack)
                            flags[flen++] = 'A';
                        if (tcphdr->fin)
                            flags[flen++] = 'F';
                        flags[flen] = 0;

                        log_android(tcphdr->fin ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
                                    "TCP %s %s from %s/%u to %s/%u state %s seq %u/%u ack %u/%u data %d",
                                    msg, flags,
                                    source, ntohs(tcphdr->source),
                                    dest, ntohs(cur->dest),
                                    strstate(cur->state),
                                    ntohl(tcphdr->seq) - cur->remote_start,
                                    cur->remote_seq - cur->remote_start,
                                    ntohl(tcphdr->ack_seq) - cur->local_start,
                                    cur->local_seq - cur->local_start,
                                    datalen);

                        return allowed;
                    }
                }
            }

            if (cur->state != oldstate || cur->local_seq != oldlocal ||
                cur->remote_seq != oldremote)
                log_android(ANDROID_LOG_INFO,
                            "TCP session from %s/%u to %s/%u new state %s local %u remote %u window %u",
                            source, ntohs(tcphdr->source),
                            dest, ntohs(cur->dest),
                            strstate(cur->state),
                            cur->local_seq - cur->local_start,
                            cur->remote_seq - cur->remote_start,
                            ntohs(tcphdr->window));
        }

#ifdef PROFILE
        gettimeofday(&end, NULL);
        mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                    (end.tv_usec - start.tv_usec) / 1000.0;
        if (mselapsed > 1)
            log_android(ANDROID_LOG_INFO, "existing session %f", mselapsed);
#endif
    }

    return 1;
}

int open_udp_socket(const struct arguments *args, const struct udp_session *cur) {
    int sock;

    // Get UDP socket
    sock = socket(cur->version == 4 ? PF_INET : PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        log_android(ANDROID_LOG_ERROR, "UDP socket error %d: %s", errno, strerror(errno));
        return -1;
    }

    // Protect socket
    if (protect_socket(args, sock) < 0)
        return -1;

    // Check for broadcast
    if (cur->version == 4) {
        uint32_t broadcast4 = INADDR_BROADCAST;
        if (memcmp(&cur->daddr.ip4, &broadcast4, sizeof(broadcast4)) == 0) {
            log_android(ANDROID_LOG_WARN, "UDP broadcast");
            int on = 1;
            if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)))
                log_android(ANDROID_LOG_ERROR, "UDP setsockopt SO_BROADCAST error %d: %s",
                            errno, strerror(errno));
        }
    } else {
        // TODO IPv6 broadcast
        // ffX2::0/16
        /*
        struct ipv6_mreq mreq6;
        mreq6->ipv6mr_multiaddr;
        mreq6->ipv6mr_ifindex;
        if (setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, (char *) &mreq6, sizeof(mreq6)))
            log_android(ANDROID_LOG_ERROR, "UDP setsockopt IPV6_ADD_MEMBERSHIP error %d: %s",
                        errno, strerror(errno));
        */
    }

    // Set blocking
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        log_android(ANDROID_LOG_ERROR, "fcntl socket ~O_NONBLOCK error %d: %s",
                    errno, strerror(errno));
        return -1;
    }

    return sock;
}

int open_tcp_socket(const struct arguments *args, const struct tcp_session *cur) {
    int sock;

    // Get TCP socket
    // TODO socket options?
    if ((sock = socket(cur->version == 4 ? PF_INET : PF_INET6, SOCK_STREAM, 0)) < 0) {
        log_android(ANDROID_LOG_ERROR, "socket error %d: %s", errno, strerror(errno));
        return -1;
    }

    // Protect
    if (protect_socket(args, sock) < 0)
        return -1;

    // Set non blocking
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_android(ANDROID_LOG_ERROR, "fcntl socket O_NONBLOCK error %d: %s",
                    errno, strerror(errno));
        return -1;
    }

    // Build target address
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    if (cur->version == 4) {
        addr4.sin_family = (__kernel_sa_family_t) AF_INET;
        addr4.sin_addr.s_addr = (__be32) cur->daddr.ip4;
        addr4.sin_port = cur->dest;
    } else {
        addr6.sin6_family = (__kernel_sa_family_t) AF_INET6;
        memcpy(&addr6.sin6_addr, &cur->daddr.ip6, 16);
        addr6.sin6_port = cur->dest;
    }

    // Initiate connect
    int err = connect(sock,
                      (const struct sockaddr *) (cur->version == 4 ? &addr4 : &addr6),
                      (socklen_t) (cur->version == 4
                                   ? sizeof(struct sockaddr_in)
                                   : sizeof(struct sockaddr_in6)));
    if (err < 0 && errno != EINPROGRESS) {
        log_android(ANDROID_LOG_ERROR, "connect error %d: %s", errno, strerror(errno));
        return -1;
    }

    // Set blocking
    if (fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        log_android(ANDROID_LOG_ERROR, "fcntl socket ~O_NONBLOCK error %d: %s",
                    errno, strerror(errno));
        return -1;
    }

    return sock;
}

int32_t get_local_port(const int sock) {
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    if (getsockname(sock, (struct sockaddr *) &sin, &len) < 0) {
        log_android(ANDROID_LOG_ERROR, "getsockname error %d: %s", errno, strerror(errno));
        return -1;
    } else
        return ntohs(sin.sin_port);
}

int write_syn_ack(const struct arguments *args, struct tcp_session *cur) {
    if (write_tcp(args, cur, NULL, 0, 1, 1, 1, 0, 0) < 0) {
        log_android(ANDROID_LOG_ERROR, "write SYN+ACK error %d: %s",
                    errno, strerror((errno)));
        cur->state = TCP_TIME_WAIT;
        return -1;
    }
    return 0;
}

int write_ack(const struct arguments *args, struct tcp_session *cur, size_t bytes) {
    if (write_tcp(args, cur, NULL, 0, bytes, 0, 1, 0, 0) < 0) {
        log_android(ANDROID_LOG_ERROR, "write ACK error %d: %s",
                    errno, strerror((errno)));
        cur->state = TCP_TIME_WAIT;
        return -1;
    }
    return 0;
}

int write_data(const struct arguments *args, struct tcp_session *cur,
               const uint8_t *buffer, size_t length) {
    if (write_tcp(args, cur, buffer, length, 0, 0, 1, 0, 0) < 0) {
        log_android(ANDROID_LOG_ERROR, "write data ACK error %d: %s", errno, strerror((errno)));
        cur->state = TCP_TIME_WAIT;
        return -1;
    }
    return 0;
}

int write_fin_ack(const struct arguments *args, struct tcp_session *cur, size_t bytes) {
    if (write_tcp(args, cur, NULL, 0, bytes, 0, 1, 1, 0) < 0) {
        log_android(ANDROID_LOG_ERROR, "write FIN+ACK error %d: %s", errno, strerror((errno)));
        cur->state = TCP_TIME_WAIT;
        return -1;
    }
    return 0;
}

void write_rst(const struct arguments *args, struct tcp_session *cur) {
    if (write_tcp(args, cur, NULL, 0, 0, 0, 0, 0, 1) < 0)
        log_android(ANDROID_LOG_ERROR, "write RST error %d: %s", errno, strerror((errno)));
    if (cur->state != TCP_CLOSE)
        cur->state = TCP_TIME_WAIT;
}

// TODO common UDP/TCP

ssize_t write_udp(const struct arguments *args, const struct udp_session *cur,
                  uint8_t *data, size_t datalen) {
#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif

    size_t len;
    u_int8_t *buffer;
    struct udphdr *udp;
    uint16_t csum;
    char source[INET6_ADDRSTRLEN + 1];
    char dest[INET6_ADDRSTRLEN + 1];

    // Build packet
    if (cur->version == 4) {
        len = sizeof(struct iphdr) + sizeof(struct udphdr) + datalen;
        buffer = malloc(len);
        struct iphdr *ip4 = (struct iphdr *) buffer;
        udp = (struct udphdr *) (buffer + sizeof(struct iphdr));
        if (datalen)
            memcpy(buffer + sizeof(struct iphdr) + sizeof(struct udphdr), data, datalen);

        // Build IP4 header
        memset(ip4, 0, sizeof(struct iphdr));
        ip4->version = 4;
        ip4->ihl = sizeof(struct iphdr) >> 2;
        ip4->tot_len = htons(len);
        ip4->ttl = IPDEFTTL;
        ip4->protocol = IPPROTO_UDP;
        ip4->saddr = cur->daddr.ip4;
        ip4->daddr = cur->saddr.ip4;

        // Calculate IP4 checksum
        ip4->check = ~calc_checksum(0, (uint8_t *) ip4, sizeof(struct iphdr));

        // Calculate UDP4 checksum
        struct ippseudo pseudo;
        memset(&pseudo, 0, sizeof(struct ippseudo));
        pseudo.ippseudo_src.s_addr = (__be32) ip4->saddr;
        pseudo.ippseudo_dst.s_addr = (__be32) ip4->daddr;
        pseudo.ippseudo_p = ip4->protocol;
        pseudo.ippseudo_len = htons(sizeof(struct udphdr) + datalen);

        csum = calc_checksum(0, (uint8_t *) &pseudo, sizeof(struct ippseudo));
    }
    else {
        len = sizeof(struct ip6_hdr) + sizeof(struct udphdr) + datalen;
        buffer = malloc(len);
        struct ip6_hdr *ip6 = (struct ip6_hdr *) buffer;
        udp = (struct udphdr *) (buffer + sizeof(struct ip6_hdr));
        if (datalen)
            memcpy(buffer + sizeof(struct ip6_hdr) + sizeof(struct udphdr), data, datalen);

        // Build IP6 header
        memset(ip6, 0, sizeof(struct ip6_hdr));
        ip6->ip6_ctlun.ip6_un1.ip6_un1_flow = 0;
        ip6->ip6_ctlun.ip6_un1.ip6_un1_plen = htons(len - sizeof(struct ip6_hdr));
        ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_UDP;
        ip6->ip6_ctlun.ip6_un1.ip6_un1_hlim = IPDEFTTL;
        ip6->ip6_ctlun.ip6_un2_vfc = IPV6_VERSION;
        memcpy(&(ip6->ip6_src), &cur->daddr.ip6, 16);
        memcpy(&(ip6->ip6_dst), &cur->saddr.ip6, 16);

        // Calculate UDP6 checksum
        struct ip6_hdr_pseudo pseudo;
        memset(&pseudo, 0, sizeof(struct ip6_hdr_pseudo));
        memcpy(&pseudo.ip6ph_src, &ip6->ip6_dst, 16);
        memcpy(&pseudo.ip6ph_dst, &ip6->ip6_src, 16);
        pseudo.ip6ph_len = ip6->ip6_ctlun.ip6_un1.ip6_un1_plen;
        pseudo.ip6ph_nxt = ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt;

        csum = calc_checksum(0, (uint8_t *) &pseudo, sizeof(struct ip6_hdr_pseudo));
    }

    // Build UDP header
    memset(udp, 0, sizeof(struct udphdr));
    udp->source = cur->dest;
    udp->dest = cur->source;
    udp->len = htons(sizeof(struct udphdr) + datalen);

    // Continue checksum
    csum = calc_checksum(csum, (uint8_t *) udp, sizeof(struct udphdr));
    csum = calc_checksum(csum, data, datalen);
    udp->check = ~csum;

    inet_ntop(cur->version == 4 ? AF_INET : AF_INET6,
              cur->version == 4 ? &cur->saddr.ip4 : &cur->saddr.ip6, source, sizeof(source));
    inet_ntop(cur->version == 4 ? AF_INET : AF_INET6,
              cur->version == 4 ? &cur->daddr.ip4 : &cur->daddr.ip6, dest, sizeof(dest));

    // Send packet
    log_android(ANDROID_LOG_DEBUG,
                "UDP sending to tun %d from %s/%u to %s/%u data %u",
                args->tun, source, ntohs(cur->source), dest, ntohs(cur->dest), len);

    ssize_t res = write(args->tun, buffer, len);

#ifdef PROFILE
    gettimeofday(&end, NULL);
    mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_usec - start.tv_usec) / 1000.0;
    if (mselapsed > 1)
        log_android(ANDROID_LOG_INFO, "tun UDP write %f", mselapsed);
#endif

    if (res >= 0) {
        if (args->log && ntohs(cur->dest) != 53)
            log_packet(args, cur->version, IPPROTO_UDP, "",
                       dest, ntohs(udp->dest), source, ntohs(udp->source), "", cur->uid, 1);

        // Write pcap record
        if (pcap_file != NULL)
            write_pcap_rec(buffer, res);
    }

    free(buffer);

    if (res != len) {
        log_android(ANDROID_LOG_ERROR, "write %d wrote %d", res, len);
        return -1;
    }

    return res;
}

ssize_t write_tcp(const struct arguments *args, const struct tcp_session *cur,
                  const uint8_t *data, size_t datalen, size_t confirm,
                  int syn, int ack, int fin, int rst) {
#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif

    size_t len;
    u_int8_t *buffer;
    struct tcphdr *tcp;
    uint16_t csum;
    char source[INET6_ADDRSTRLEN + 1];
    char dest[INET6_ADDRSTRLEN + 1];

    // Build packet
    if (cur->version == 4) {
        len = sizeof(struct iphdr) + sizeof(struct tcphdr) + datalen;
        buffer = malloc(len);
        struct iphdr *ip4 = (struct iphdr *) buffer;
        tcp = (struct tcphdr *) (buffer + sizeof(struct iphdr));
        if (datalen)
            memcpy(buffer + sizeof(struct iphdr) + sizeof(struct tcphdr), data, datalen);

        // Build IP4 header
        memset(ip4, 0, sizeof(struct iphdr));
        ip4->version = 4;
        ip4->ihl = sizeof(struct iphdr) >> 2;
        ip4->tot_len = htons(len);
        ip4->ttl = IPDEFTTL;
        ip4->protocol = IPPROTO_TCP;
        ip4->saddr = cur->daddr.ip4;
        ip4->daddr = cur->saddr.ip4;

        // Calculate IP4 checksum
        ip4->check = ~calc_checksum(0, (uint8_t *) ip4, sizeof(struct iphdr));

        // Calculate TCP4 checksum
        struct ippseudo pseudo;
        memset(&pseudo, 0, sizeof(struct ippseudo));
        pseudo.ippseudo_src.s_addr = (__be32) ip4->saddr;
        pseudo.ippseudo_dst.s_addr = (__be32) ip4->daddr;
        pseudo.ippseudo_p = ip4->protocol;
        pseudo.ippseudo_len = htons(sizeof(struct tcphdr) + datalen);

        csum = calc_checksum(0, (uint8_t *) &pseudo, sizeof(struct ippseudo));
    }
    else {
        len = sizeof(struct ip6_hdr) + sizeof(struct tcphdr) + datalen;
        buffer = malloc(len);
        struct ip6_hdr *ip6 = (struct ip6_hdr *) buffer;
        tcp = (struct tcphdr *) (buffer + sizeof(struct ip6_hdr));
        if (datalen)
            memcpy(buffer + sizeof(struct ip6_hdr) + sizeof(struct tcphdr), data, datalen);

        // Build IP6 header
        memset(ip6, 0, sizeof(struct ip6_hdr));
        ip6->ip6_ctlun.ip6_un1.ip6_un1_plen = htons(len - sizeof(struct ip6_hdr));
        ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt = IPPROTO_TCP;
        ip6->ip6_ctlun.ip6_un1.ip6_un1_hlim = IPDEFTTL;
        ip6->ip6_ctlun.ip6_un2_vfc = 0x60;
        memcpy(&(ip6->ip6_src), &cur->daddr.ip6, 16);
        memcpy(&(ip6->ip6_dst), &cur->saddr.ip6, 16);

        // Calculate TCP6 checksum
        struct ip6_hdr_pseudo pseudo;
        memset(&pseudo, 0, sizeof(struct ip6_hdr_pseudo));
        memcpy(&pseudo.ip6ph_src, &ip6->ip6_dst, 16);
        memcpy(&pseudo.ip6ph_dst, &ip6->ip6_src, 16);
        pseudo.ip6ph_len = ip6->ip6_ctlun.ip6_un1.ip6_un1_plen;
        pseudo.ip6ph_nxt = ip6->ip6_ctlun.ip6_un1.ip6_un1_nxt;

        csum = calc_checksum(0, (uint8_t *) &pseudo, sizeof(struct ip6_hdr_pseudo));
    }

    // Build TCP header
    memset(tcp, 0, sizeof(struct tcphdr));
    tcp->source = cur->dest;
    tcp->dest = cur->source;
    tcp->seq = htonl(cur->local_seq);
    tcp->ack_seq = htonl((uint32_t) (cur->remote_seq + confirm));
    tcp->doff = sizeof(struct tcphdr) >> 2;
    tcp->syn = (__u16) syn;
    tcp->ack = (__u16) ack;
    tcp->fin = (__u16) fin;
    tcp->rst = (__u16) rst;
    tcp->window = htons(TCP_RECV_WINDOW);
    tcp->urg_ptr;

    if (!tcp->ack)
        tcp->ack_seq = 0;

    // Continue checksum
    csum = calc_checksum(csum, (uint8_t *) tcp, sizeof(struct tcphdr));
    csum = calc_checksum(csum, data, datalen);
    tcp->check = ~csum;

    inet_ntop(cur->version == 4 ? AF_INET : AF_INET6,
              cur->version == 4 ? &cur->saddr.ip4 : &cur->saddr.ip6, source, sizeof(source));
    inet_ntop(cur->version == 4 ? AF_INET : AF_INET6,
              cur->version == 4 ? &cur->daddr.ip4 : &cur->daddr.ip6, dest, sizeof(dest));

    // Send packet
    log_android(ANDROID_LOG_DEBUG,
                "TCP sending%s%s%s%s to tun %s/%u seq %u ack %u data %u confirm %u",
                (tcp->syn ? " SYN" : ""),
                (tcp->ack ? " ACK" : ""),
                (tcp->fin ? " FIN" : ""),
                (tcp->rst ? " RST" : ""),
                dest, ntohs(tcp->dest),
                ntohl(tcp->seq) - cur->local_start,
                ntohl(tcp->ack_seq) - cur->remote_start,
                datalen, confirm);

    ssize_t res = write(args->tun, buffer, len);

#ifdef PROFILE
    gettimeofday(&end, NULL);
    mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_usec - start.tv_usec) / 1000.0;
    if (mselapsed > 1)
        log_android(ANDROID_LOG_INFO, "tun TCP write %f", mselapsed);
#endif

    // Write pcap record
    if (res >= 0 && pcap_file != NULL)
        write_pcap_rec(buffer, res);

    free(buffer);

    if (res != len) {
        log_android(ANDROID_LOG_ERROR, "TCP write %d wrote %d", res, len);
        return -1;
    }

    return res;
}

uint8_t char2nible(const char c) {
    if (c >= '0' && c <= '9') return (uint8_t) (c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t) ((c - 'a') + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t) ((c - 'A') + 10);
    return 255;
}

void hex2bytes(const char *hex, uint8_t *buffer) {
    size_t len = strlen(hex);
    for (int i = 0; i < len; i += 2)
        buffer[i / 2] = (char2nible(hex[i]) << 4) | char2nible(hex[i + 1]);
}

jint get_uid(const int protocol, const int version,
             const void *saddr, const uint16_t sport, int dump) {
    char line[250];
    char hex[16 * 2 + 1];
    int fields;
    uint8_t addr4[4];
    uint8_t addr6[16];
    int port;
    jint uid = -1;

#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif

    // Get proc file name
    char *fn = NULL;
    if (protocol == IPPROTO_TCP)
        fn = (version == 4 ? "/proc/net/tcp" : "/proc/net/tcp6");
    else if (protocol == IPPROTO_UDP)
        fn = (version == 4 ? "/proc/net/udp" : "/proc/net/udp6");
    else
        return uid;

    if (dump) {
        char source[INET6_ADDRSTRLEN + 1];
        inet_ntop(version == 4 ? AF_INET : AF_INET6, saddr, source, sizeof(source));
        log_android(ANDROID_LOG_INFO, "Searching %s/%u in %s", source, sport, fn);
    }

    // Open proc file
    FILE *fd = fopen(fn, "r");
    if (fd == NULL) {
        log_android(ANDROID_LOG_ERROR, "fopen %s error %d: %s", fn, errno, strerror(errno));
        return uid;
    }

    // Scan proc file
    jint u;
    int i = 0;
    while (fgets(line, sizeof(line), fd) != NULL) {
        if (i++) {
            if (version == 4)
                fields = sscanf(
                        line,
                        "%*d: %8s:%X %*X:%*X %*X %*lX:%*lX %*X:%*X %*X %d %*d %*ld",
                        hex, &port, &u);
            else
                fields = sscanf(
                        line,
                        "%*d: %32s:%X %*X:%*X %*X %*lX:%*lX %*X:%*X %*X %d %*d %*ld",
                        hex, &port, &u);
            if (fields == 3) {
                hex2bytes(hex, version == 4 ? addr4 : addr6);
                if (version == 4)
                    ((uint32_t *) addr4)[0] = htonl(((uint32_t *) addr4)[0]);
                for (int w = 0; w < 4; w++)
                    ((uint32_t *) addr6)[w] = htonl(((uint32_t *) addr6)[w]);

                if (dump) {
                    char source[INET6_ADDRSTRLEN + 1];
                    inet_ntop(version == 4 ? AF_INET : AF_INET6,
                              version == 4 ? addr4 : addr6,
                              source, sizeof(source));
                    log_android(ANDROID_LOG_INFO, "%s/%u %d", source, sport, u);
                }

                if (port == sport) {
                    if (memcmp(version == 4 ? addr4 : addr6, saddr, version == 4 ? 4 : 16) ==
                        0) {
                        uid = u;
                        break;
                    }
                }
            } else
                log_android(ANDROID_LOG_ERROR, "Invalid field #%d: %s", fields, line);
        }
    }

    if (fclose(fd))
        log_android(ANDROID_LOG_ERROR, "fclose %s error %d: %s", fn, errno, strerror(errno));

#ifdef PROFILE
    gettimeofday(&end, NULL);
    mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_usec - start.tv_usec) / 1000.0;
    if (mselapsed > 1)
        log_android(ANDROID_LOG_INFO, "get uid ip %f", mselapsed);
#endif

    return uid;
}

int protect_socket(const struct arguments *args, int socket) {
    jclass cls = (*args->env)->GetObjectClass(args->env, args->instance);
    jmethodID mid = jniGetMethodID(args->env, cls, "protect", "(I)Z");

    jboolean isProtected = (*args->env)->CallBooleanMethod(args->env, args->instance, mid, socket);
    jniCheckException(args->env);

    if (!isProtected) {
        log_android(ANDROID_LOG_ERROR, "protect socket failed");
        return -1;
    }

    (*args->env)->DeleteLocalRef(args->env, cls);

    return 0;
}

uint16_t calc_checksum(uint16_t start, const uint8_t *buffer, size_t length) {
    register uint32_t sum = start;
    register uint16_t *buf = (uint16_t *) buffer;
    register size_t len = length;

    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }

    if (len > 0)
        sum += *((uint8_t *) buf);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t) sum;
}

// http://docs.oracle.com/javase/7/docs/technotes/guides/jni/spec/functions.html
// http://journals.ecs.soton.ac.uk/java/tutorial/native1.1/implementing/index.html

jobject jniGlobalRef(JNIEnv *env, jobject cls) {
    jobject gcls = (*env)->NewGlobalRef(env, cls);
    if (gcls == NULL)
        log_android(ANDROID_LOG_ERROR, "Global ref failed (out of memory?)");
    return gcls;
}

jclass jniFindClass(JNIEnv *env, const char *name) {
    jclass cls = (*env)->FindClass(env, name);
    if (cls == NULL)
        log_android(ANDROID_LOG_ERROR, "Class %s not found", name);
    else
        jniCheckException(env);
    return cls;
}

jmethodID method_protect = NULL;
jmethodID method_logPacket = NULL;

jmethodID jniGetMethodID(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (strcmp(name, "protect") == 0 && method_protect != NULL)
        return method_protect;
    if (strcmp(name, "logPacket") == 0 && method_logPacket != NULL)
        return method_logPacket;

    jmethodID method = (*env)->GetMethodID(env, cls, name, signature);
    if (method == NULL)
        log_android(ANDROID_LOG_ERROR, "Method %s%s", name, signature);
    else {
        if (strcmp(name, "protect") == 0) {
            method_protect = method;
            log_android(ANDROID_LOG_INFO, "Cached method ID protect");
        }
        else if (strcmp(name, "logPacket") == 0) {
            method_logPacket = method;
            log_android(ANDROID_LOG_INFO, "Cached method ID logPacket");
        }
    }
    return method;
}

jfieldID jniGetFieldID(JNIEnv *env, jclass cls, const char *name, const char *type) {
    jfieldID field = (*env)->GetFieldID(env, cls, name, type);
    if (field == NULL)
        log_android(ANDROID_LOG_ERROR, "Field %s type %s not found", name, type);
    return field;
}

jobject jniNewObject(JNIEnv *env, jclass cls, jmethodID constructor, const char *name) {
    jobject object = (*env)->NewObject(env, cls, constructor);
    if (object == NULL)
        log_android(ANDROID_LOG_ERROR, "Create object %s failed", name);
    else
        jniCheckException(env);
    return object;
}

int jniCheckException(JNIEnv *env) {
    jthrowable ex = (*env)->ExceptionOccurred(env);
    if (ex) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, ex);
        return 1;
    }
    return 0;
}

typedef int (*PFN_SYS_PROP_GET)(const char *, char *);

int __system_property_get(const char *name, char *value) {
    static PFN_SYS_PROP_GET __real_system_property_get = NULL;
    if (!__real_system_property_get) {
        void *handle = dlopen("libc.so", RTLD_NOLOAD);
        if (!handle)
            log_android(ANDROID_LOG_ERROR, "dlopen(libc.so): %s", dlerror());
        else {
            __real_system_property_get = (PFN_SYS_PROP_GET) dlsym(handle, "__system_property_get");
            if (!__real_system_property_get)
                log_android(ANDROID_LOG_ERROR, "dlsym(__system_property_get()): %s", dlerror());
        }
    }
    return (*__real_system_property_get)(name, value);
}

void log_android(int prio, const char *fmt, ...) {
    if (prio >= loglevel) {
        char line[1024];
        va_list argptr;
        va_start(argptr, fmt);
        vsprintf(line, fmt, argptr);
        __android_log_print(prio, TAG, line);
        va_end(argptr);
    }
}

void log_packet(
        const struct arguments *args,
        jint version,
        jint protocol,
        const char *flags,
        const char *source,
        jint sport,
        const char *dest,
        jint dport,
        const char *data,
        jint uid,
        jboolean allowed) {
#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif

    JNIEnv *env = args->env;
    jclass clsService = (*env)->GetObjectClass(env, args->instance);

    const char *signature = "(Leu/faircode/netguard/Packet;)V";
    jmethodID logPacket = jniGetMethodID(env, clsService, "logPacket", signature);

    const char *packet = "eu/faircode/netguard/Packet";
    jmethodID initPacket = jniGetMethodID(env, clsPacket, "<init>", "()V");
    jobject objPacket = jniNewObject(env, clsPacket, initPacket, packet);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    jlong t = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    jstring jflags = (*env)->NewStringUTF(env, flags);
    jstring jsource = (*env)->NewStringUTF(env, source);
    jstring jdest = (*env)->NewStringUTF(env, dest);
    jstring jdata = (*env)->NewStringUTF(env, data);

    const char *string = "Ljava/lang/String;";
    (*env)->SetLongField(env, objPacket, jniGetFieldID(env, clsPacket, "time", "J"), t);
    (*env)->SetIntField(env, objPacket, jniGetFieldID(env, clsPacket, "version", "I"), version);
    (*env)->SetIntField(env, objPacket, jniGetFieldID(env, clsPacket, "protocol", "I"), protocol);
    (*env)->SetObjectField(env, objPacket, jniGetFieldID(env, clsPacket, "flags", string), jflags);
    (*env)->SetObjectField(env, objPacket, jniGetFieldID(env, clsPacket, "saddr", string), jsource);
    (*env)->SetIntField(env, objPacket, jniGetFieldID(env, clsPacket, "sport", "I"), sport);
    (*env)->SetObjectField(env, objPacket, jniGetFieldID(env, clsPacket, "daddr", string), jdest);
    (*env)->SetIntField(env, objPacket, jniGetFieldID(env, clsPacket, "dport", "I"), dport);
    (*env)->SetObjectField(env, objPacket, jniGetFieldID(env, clsPacket, "data", string), jdata);
    (*env)->SetIntField(env, objPacket, jniGetFieldID(env, clsPacket, "uid", "I"), uid);
    (*env)->SetBooleanField(env, objPacket, jniGetFieldID(env, clsPacket, "allowed", "Z"), allowed);

    (*env)->CallVoidMethod(env, args->instance, logPacket, objPacket);
    jniCheckException(env);

    (*env)->DeleteLocalRef(env, jdata);
    (*env)->DeleteLocalRef(env, jdest);
    (*env)->DeleteLocalRef(env, jsource);
    (*env)->DeleteLocalRef(env, jflags);
    (*env)->DeleteLocalRef(env, objPacket);
    (*env)->DeleteLocalRef(env, clsService);

#ifdef PROFILE
    gettimeofday(&end, NULL);
    mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_usec - start.tv_usec) / 1000.0;
    if (mselapsed > 1)
        log_android(ANDROID_LOG_INFO, "log java %f", mselapsed);
#endif
}

void write_pcap_hdr() {
    struct pcap_hdr_s pcap_hdr;
    pcap_hdr.magic_number = 0xa1b2c3d4;
    pcap_hdr.version_major = 2;
    pcap_hdr.version_minor = 4;
    pcap_hdr.thiszone = 0;
    pcap_hdr.sigfigs = 0;
    pcap_hdr.snaplen = MAX_PCAP_RECORD;
    pcap_hdr.network = LINKTYPE_RAW;
    write_pcap(&pcap_hdr, sizeof(struct pcap_hdr_s));
}

void write_pcap_rec(const uint8_t *buffer, size_t length) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts))
        log_android(ANDROID_LOG_ERROR, "clock_gettime error %d: %s", errno, strerror(errno));

    size_t plen = (length < MAX_PCAP_RECORD ? length : MAX_PCAP_RECORD);
    struct pcaprec_hdr_s pcap_rec;

    pcap_rec.ts_sec = (guint32_t) ts.tv_sec;
    pcap_rec.ts_usec = (guint32_t) (ts.tv_nsec / 1000);
    pcap_rec.incl_len = (guint32_t) plen;
    pcap_rec.orig_len = (guint32_t) length;

    write_pcap(&pcap_rec, sizeof(struct pcaprec_hdr_s));
    write_pcap(buffer, plen);
}

void write_pcap(const void *ptr, size_t len) {
#ifdef PROFILE
    float mselapsed;
    struct timeval start, end;
    gettimeofday(&start, NULL);
#endif

    if (fwrite(ptr, len, 1, pcap_file) < 1)
        log_android(ANDROID_LOG_ERROR, "PCAP fwrite error %d: %s", errno, strerror(errno));
    else {
        long fsize = ftell(pcap_file);
        log_android(ANDROID_LOG_DEBUG, "PCAP wrote %d @%ld", len, fsize);

        if (fsize > MAX_PCAP_FILE) {
            log_android(ANDROID_LOG_WARN, "PCAP truncate @%ld", fsize);
            if (ftruncate(fileno(pcap_file), sizeof(struct pcap_hdr_s)))
                log_android(ANDROID_LOG_ERROR, "PCAP ftruncate error %d: %s",
                            errno, strerror(errno));
            else {
                if (!lseek(fileno(pcap_file), sizeof(struct pcap_hdr_s), SEEK_SET))
                    log_android(ANDROID_LOG_ERROR, "PCAP ftruncate error %d: %s",
                                errno, strerror(errno));
            }
        }
    }

#ifdef PROFILE
    gettimeofday(&end, NULL);
    mselapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                (end.tv_usec - start.tv_usec) / 1000.0;
    if (mselapsed > 1)
        log_android(ANDROID_LOG_INFO, "pcap write %f", mselapsed);
#endif
}

char *trim(char *str) {
    while (isspace(*str))
        str++;
    if (*str == 0)
        return str;

    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end))
        end--;
    *(end + 1) = 0;
    return str;
}

void read_hosts(const char *name, struct arguments *args) {
    log_android(ANDROID_LOG_INFO, "Reading %s", name);

    args->hcount = 0;
    args->hosts = NULL;

    FILE *hosts;
    if ((hosts = fopen(name, "r")) == NULL) {
        log_android(ANDROID_LOG_ERROR, "fopen(%s) error %d: %s", name, errno, strerror(errno));
        return;
    }

    char buffer[160];
    while (fgets(buffer, sizeof(buffer), hosts)) {
        char *hash = strchr(buffer, '#');
        if (hash)
            *hash = 0;

        char *host = trim(buffer);
        while (*host && !isspace(*host))
            host++;

        if (isspace(*host)) {
            host++;
            if (*host && strcmp(host, "localhost")) {
                args->hosts = realloc(args->hosts, sizeof(char *) * (args->hcount + 1));
                args->hosts[args->hcount] = malloc(strlen(host) + 1);
                strcpy(args->hosts[args->hcount], host);
                args->hcount++;
            }
        }
    }

    if (fclose(hosts))
        log_android(ANDROID_LOG_ERROR, "fclose(%s) error %d: %s", name, errno, strerror(errno));

    for (int i = 0; i < args->hcount; i++)
        log_android(ANDROID_LOG_VERBOSE, "host '%s'", args->hosts[i]);
}

const char *strstate(const int state) {
    switch (state) {
        case TCP_ESTABLISHED:
            return "ESTABLISHED";
        case TCP_SYN_SENT:
            return "SYN_SENT";
        case TCP_SYN_RECV:
            return "SYN_RECV";
        case TCP_FIN_WAIT1:
            return "FIN_WAIT1";
        case TCP_FIN_WAIT2:
            return "FIN_WAIT2";
        case TCP_TIME_WAIT:
            return "TIME_WAIT";
        case TCP_CLOSE:
            return "CLOSE";
        case TCP_CLOSE_WAIT:
            return "CLOSE_WAIT";
        case TCP_LAST_ACK:
            return "LAST_ACK";
        case TCP_LISTEN:
            return "LISTEN";
        case  TCP_CLOSING:
            return "CLOSING";
        default:
            return "UNKNOWN";
    }
}

char *hex(const u_int8_t *data, const size_t len) {
    char hex_str[] = "0123456789ABCDEF";

    char *hexout;
    hexout = (char *) malloc(len * 3 + 1); // TODO free

    for (size_t i = 0; i < len; i++) {
        hexout[i * 3 + 0] = hex_str[(data[i] >> 4) & 0x0F];
        hexout[i * 3 + 1] = hex_str[(data[i]) & 0x0F];
        hexout[i * 3 + 2] = ' ';
    }
    hexout[len * 3] = 0;

    return hexout;
}
