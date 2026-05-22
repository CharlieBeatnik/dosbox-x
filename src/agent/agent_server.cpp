/*
 *  Copyright (C) 2002-2025  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Agent control channel — SDL_net TCP accept/recv/send and per-client outbox.
 *
 * Phase-1 design: one listener, at most one connected client. A second
 * concurrent connection is rejected with a single `{"event":"busy"}` line
 * and closed.
 *
 * Loopback only. The `listen` config string is parsed as "addr:port"; the
 * address must resolve to 127.0.0.1. Port 0 selects an ephemeral port. */

#include "config.h"

#if C_DEBUG

#include "agent_internal.h"
#include "logging.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
# include <winsock2.h>   /* SOCKET / getsockname for the ephemeral-port readback */
#else
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

#if defined(C_SDL2_NET) && C_SDL2_NET
#include <SDL2/SDL_net.h>
#else
#include <SDL_net.h>
#endif

/* Forward decl from the serial-port subsystem — already used to lazily
 * SDLNet_Init() the library exactly once. */
bool NetWrapper_InitializeSDLNet();

namespace agent {

namespace {

/* 1 MB per-client outbox cap. On overflow we drop the offending line and
 * remember to send an `agent.overflow` event the next time the outbox has
 * room. */
constexpr size_t OUTBOX_CAP_BYTES = 1024 * 1024;

struct Client {
    TCPsocket           sock     = nullptr;
    std::string         rxBuffer;
    std::deque<std::string> outbox;
    size_t              outboxBytes = 0;
    bool                overflowPending = false;
    bool                logSubscribed = false;
};

struct Server {
    bool                active = false;
    TCPsocket           listener = nullptr;
    SDLNet_SocketSet    sockset  = nullptr;
    uint16_t            listenPort = 0;
    std::string         portfile;
    std::string         authToken;
    /* Only one client in Phase 1. Kept as a pointer so we can later widen
     * to std::vector<std::unique_ptr<Client>> without much churn. */
    std::unique_ptr<Client> client;
};

Server g;

void closeClient() {
    if (!g.client) return;
    if (g.client->sock) {
        if (g.sockset) SDLNet_TCP_DelSocket(g.sockset, g.client->sock);
        SDLNet_TCP_Close(g.client->sock);
    }
    g.client.reset();
}

void writePortFile() {
    if (g.portfile.empty()) return;
    FILE *f = fopen(g.portfile.c_str(), "wb");
    if (!f) {
        LOG(LOG_MISC, LOG_WARN)("agent: cannot write portfile '%s'", g.portfile.c_str());
        return;
    }
    fprintf(f, "%u\n", static_cast<unsigned>(g.listenPort));
    fclose(f);
}

void removePortFile() {
    if (g.portfile.empty()) return;
    remove(g.portfile.c_str());
}

/* Split "host:port" into its parts. Accepts a bare "host" (port defaults
 * to 0) and a bare ":port" (host defaults to 127.0.0.1). */
void parseListen(const std::string &s, std::string &host, uint16_t &port) {
    host = "127.0.0.1";
    port = 0;
    if (s.empty()) return;
    size_t colon = s.rfind(':');
    if (colon == std::string::npos) {
        host = s;
    } else {
        if (colon > 0) host = s.substr(0, colon);
        if (colon + 1 < s.size()) port = static_cast<uint16_t>(atoi(s.c_str() + colon + 1));
    }
}

}  /* anonymous namespace */

bool serverActive() { return g.active; }

void serverStart(const std::string &listen, const std::string &portfile,
                 const std::string &auth_token)
{
    if (g.active) return;

    if (!NetWrapper_InitializeSDLNet()) {
        LOG(LOG_MISC, LOG_WARN)("agent: SDLNet_Init failed");
        return;
    }

    std::string host;
    uint16_t    port;
    parseListen(listen, host, port);

    /* Validate the requested listen address resolves to loopback. We
     * resolve the host (so "localhost" works) then check the result.
     * SDLNet_TCP_Open treats any non-INADDR_ANY/NONE address as a *client*
     * connect target, so we cannot pass 127.0.0.1 to it directly to bind.
     * Instead we resolve once for validation, then a second time with a
     * NULL hostname (= INADDR_ANY) to actually open the listener. Loopback
     * enforcement then happens per-connection in acceptIfReady(). */
    IPaddress checkAddr;
    if (SDLNet_ResolveHost(&checkAddr, host.c_str(), port) != 0) {
        LOG(LOG_MISC, LOG_WARN)("agent: cannot resolve listen host '%s'", host.c_str());
        return;
    }
    uint8_t firstByte = static_cast<uint8_t>(checkAddr.host & 0xFF);
    if (firstByte != 127) {
        LOG(LOG_MISC, LOG_WARN)("agent: listen address '%s' is not loopback; refusing", host.c_str());
        return;
    }

    IPaddress bindAddr;
    if (SDLNet_ResolveHost(&bindAddr, NULL, port) != 0) {
        LOG(LOG_MISC, LOG_WARN)("agent: SDLNet_ResolveHost(NULL, %u) failed: %s", static_cast<unsigned>(port), SDLNet_GetError());
        return;
    }

    g.listener = SDLNet_TCP_Open(&bindAddr);
    if (!g.listener) {
        LOG(LOG_MISC, LOG_WARN)("agent: SDLNet_TCP_Open failed: %s", SDLNet_GetError());
        return;
    }

    /* If port 0 was requested, ask the kernel for the bound port. SDL_net
     * has no public getter and its internal `localAddress` field is never
     * populated for server sockets — so we mirror the _TCPsocket struct
     * to reach the underlying `channel` (the native SOCKET / fd) and call
     * getsockname() ourselves. The layout is verbatim from
     * vs/sdlnet/SDLnetTCP.c and matches the `_TCPsocketX` mirror in
     * src/hardware/serialport/misc_util.h. */
    if (port == 0) {
        struct AgentTCPsocketLayout {
            int ready;
#if defined(_WIN32)
            SOCKET channel;
#else
            int channel;
#endif
            IPaddress remoteAddress;
            IPaddress localAddress;
            int sflag;
        };
        auto *layout = reinterpret_cast<AgentTCPsocketLayout *>(g.listener);
        struct sockaddr_in sa;
#if defined(_WIN32)
        int sa_len = sizeof(sa);
#else
        socklen_t sa_len = sizeof(sa);
#endif
        if (getsockname(layout->channel, reinterpret_cast<struct sockaddr *>(&sa), &sa_len) == 0) {
            g.listenPort = ntohs(sa.sin_port);
        } else {
            g.listenPort = 0;
        }
    } else {
        g.listenPort = port;
    }

    if (g.listenPort == 0) {
        LOG(LOG_MISC, LOG_WARN)("agent: failed to determine bound port; clients won't be able to connect");
        SDLNet_TCP_Close(g.listener); g.listener = nullptr;
        return;
    }

    g.sockset = SDLNet_AllocSocketSet(2);
    if (!g.sockset) {
        LOG(LOG_MISC, LOG_WARN)("agent: SDLNet_AllocSocketSet failed");
        SDLNet_TCP_Close(g.listener); g.listener = nullptr;
        return;
    }
    SDLNet_TCP_AddSocket(g.sockset, g.listener);

    g.portfile  = portfile;
    g.authToken = auth_token;
    g.active    = true;
    writePortFile();

    LOG(LOG_MISC, LOG_NORMAL)("agent: listening on %s:%u", host.c_str(), static_cast<unsigned>(g.listenPort));
}

void serverStop() {
    if (!g.active) return;
    closeClient();
    if (g.listener) {
        if (g.sockset) SDLNet_TCP_DelSocket(g.sockset, g.listener);
        SDLNet_TCP_Close(g.listener);
        g.listener = nullptr;
    }
    if (g.sockset) {
        SDLNet_FreeSocketSet(g.sockset);
        g.sockset = nullptr;
    }
    removePortFile();
    g.portfile.clear();
    g.authToken.clear();
    g.listenPort = 0;
    g.active = false;
}

static void enqueueLine(Client &c, const std::string &line) {
    /* Reserve one byte for the trailing newline that we add at send time. */
    if (c.outboxBytes + line.size() + 1 > OUTBOX_CAP_BYTES) {
        c.overflowPending = true;
        return;
    }
    c.outboxBytes += line.size() + 1;
    c.outbox.push_back(line);
}

void serverBroadcastLine(const std::string &line) {
    if (!g.active || !g.client) return;
    enqueueLine(*g.client, line);
}

bool serverSetLogSubscribed(bool on) {
    if (!g.active || !g.client) return false;
    g.client->logSubscribed = on;
    return true;
}

void serverEmitLogLine(const char *text) {
    if (!g.active || !g.client || !g.client->logSubscribed || !text) return;
    /* Inline JSON-encode to avoid pulling agent_json.cpp into this TU's
     * dependency chain. Strings without high bytes / control chars only
     * need quotes + a couple of escapes. DEBUG_ShowMsg's buf is already
     * newline-stripped before we get here. */
    std::string out;
    out.reserve(32 + 16);
    out.append("{\"event\":\"log.line\",\"text\":\"");
    for (const char *p = text; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out.append(esc);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.append("\"}");
    enqueueLine(*g.client, out);
}

static void acceptIfReady() {
    TCPsocket s = SDLNet_TCP_Accept(g.listener);
    if (!s) return;

    /* Reject non-loopback peers. The listener is bound to INADDR_ANY (SDL_net
     * has no portable way to bind to a specific address) so the loopback
     * restriction must be enforced here. */
    IPaddress *peer = SDLNet_TCP_GetPeerAddress(s);
    if (peer == NULL || static_cast<uint8_t>(peer->host & 0xFF) != 127) {
        LOG(LOG_MISC, LOG_WARN)("agent: rejecting non-loopback connection");
        SDLNet_TCP_Close(s);
        return;
    }

    if (g.client) {
        /* One client at a time in Phase 1. Reject. */
        const char *reply = "{\"event\":\"busy\"}\n";
        SDLNet_TCP_Send(s, reply, static_cast<int>(strlen(reply)));
        SDLNet_TCP_Close(s);
        return;
    }
    g.client.reset(new Client());
    g.client->sock = s;
    SDLNet_TCP_AddSocket(g.sockset, s);
    LOG(LOG_MISC, LOG_NORMAL)("agent: client connected");
}

static void drainOneClient() {
    if (!g.client) return;
    Client &c = *g.client;
    char buf[2048];
    int n = SDLNet_TCP_Recv(c.sock, buf, sizeof(buf));
    if (n <= 0) {
        LOG(LOG_MISC, LOG_NORMAL)("agent: client disconnected");
        closeClient();
        return;
    }
    c.rxBuffer.append(buf, static_cast<size_t>(n));

    /* Hand any complete lines to the dispatcher. \r is treated as
     * whitespace (so Windows clients sending CRLF still work). */
    for (;;) {
        size_t nl = c.rxBuffer.find('\n');
        if (nl == std::string::npos) break;
        std::string line = c.rxBuffer.substr(0, nl);
        c.rxBuffer.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string reply = dispatchLine(line);
        if (!reply.empty()) enqueueLine(c, reply);
    }
}

static void flushOutbox() {
    if (!g.client) return;
    Client &c = *g.client;

    /* If we previously dropped a line, emit the overflow notice once room
     * is available. We don't ration retries — the line is tiny. */
    if (c.overflowPending && c.outboxBytes == 0) {
        c.outbox.emplace_back("{\"event\":\"agent.overflow\"}");
        c.outboxBytes += c.outbox.back().size() + 1;
        c.overflowPending = false;
    }

    while (!c.outbox.empty()) {
        const std::string &line = c.outbox.front();
        std::string framed = line + "\n";
        int sent = SDLNet_TCP_Send(c.sock, framed.data(), static_cast<int>(framed.size()));
        if (sent < static_cast<int>(framed.size())) {
            /* SDL_net's TCP_Send is blocking and returns short only on
             * error. Treat as a disconnect. */
            LOG(LOG_MISC, LOG_NORMAL)("agent: client send failed; disconnecting");
            closeClient();
            return;
        }
        c.outboxBytes -= line.size() + 1;
        c.outbox.pop_front();
    }
}

void serverPoll() {
    if (!g.active) return;

    /* Zero-timeout check across listener + (possibly) client. */
    int ready = SDLNet_CheckSockets(g.sockset, 0);
    if (ready > 0) {
        if (SDLNet_SocketReady(g.listener)) acceptIfReady();
        if (g.client && SDLNet_SocketReady(g.client->sock)) drainOneClient();
    }
    flushOutbox();
}

}  /* namespace agent */

#endif /* C_DEBUG */
