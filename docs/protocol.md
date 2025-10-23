# Protocol for RPS bo9 (TCP)

## Overview

Text-based, line-oriented protocol over TCP. Lines terminated by `\r\n` (CRLF).
Fields separated by single spaces. Nicknames and room names MUST not contain spaces.
All messages originate from either Client or Server and are plain ASCII/UTF-8.

## General rules

* Each client must send `HELLO <nickname>` immediately after TCP connect.
* Server responds with `WELCOME <token>` or `ERR <code> <message>`.
* For each client request the server sends at least one response (ACK/ERR/etc.).
* Heartbeat: `PING` / `PONG` — client may send `PING` periodically; server replies `PONG`.
* If nothing happens, nothing is sent (except optional periodic PING).
* Max nickname length: 32 chars; allowed chars: printable non-space.
* Max room name length: 64 chars.
* Message terminator: CRLF (`\r\n`).

## Message grammar (ABNF-like)

```
LINE = COMMAND *(SP ARG) CRLF
COMMAND = uppercase-token
ARG = token / quoted-string
token = 1*<visible-printable-except-space>
quoted-string = '"' *(any-except-\" or CRLF) '"'
```

For simplicity implementation may forbid quoted strings; then tokens only.

## Client -> Server commands

* `HELLO <nickname>`

    * initial identification. Server responds `WELCOME <token>`.
    * example: `HELLO Alice\r\n`

* `LIST`

    * requests room list. Server responds:

        * `ROOM_LIST <count>`
        * `ROOM <id> <name> <players>/<max> <state>` (repeated <count> times)
    * example:

        * `ROOM_LIST 1\r\nROOM 42 room1 1/2 OPEN\r\n`

* `CREATE <room_name>`

    * create room (max players 2). Server: `ROOM_CREATED <room_id>` or `ERR`.

* `JOIN <room_id>`

    * join room id. Server: `ROOM_JOINED <room_id>` and broadcast `PLAYER_JOINED <nickname>` to room.

* `LEAVE`

    * leave current room and return to lobby. Server: `LEFT`.

* `READY`

    * mark ready in room. When both ready server sends `GAME_START`.

* `MOVE <R|P|S>`

    * send choice for current round. Server: `MOVE_ACCEPTED` or `ERR`.

* `PING`

    * heartbeat. Server: `PONG`.

* `RECONNECT <token>`

    * attempt to reattach to old session.

* `QUIT`

    * intent to quit; connection may be closed.

## Server -> Client messages

* `WELCOME <token>`

    * token is opaque session id (string). Keep locally to allow `RECONNECT`.

* `ERR <code> <message>`

    * error message. Code numeric.

* `OK <message>`

    * general positive acknowledgement.

* `ROOM_LIST <n>` and `ROOM ...` entries

* `ROOM_CREATED <room_id>`

* `ROOM_JOINED <room_id>`

* `PLAYER_JOINED <nickname>`

* `PLAYER_LEFT <nickname>`

* `GAME_START`

* `ROUND_START <round_number>`

* `MOVE_ACCEPTED`

* `ROUND_RESULT <winner|DRAW> <move_p1> <move_p2> <score_p1> <score_p2>`

    * examples:

        * `ROUND_RESULT WINNER Alice R S 3 2`
        * `ROUND_RESULT DRAW R R 2 2`

* `GAME_END <winner>`

* `PONG`

* `RECONNECT_OK <room_id> <state>`

* `PLAYER_UNAVAILABLE <nickname> <short|long>`

* `KICKED`

## Error codes

* `100` BAD_FORMAT — syntax error
* `101` INVALID_STATE — command invalid in current state
* `102` ROOM_FULL
* `103` AUTH_FAIL
* `104` UNKNOWN_ROOM
* `105` NOT_IN_ROOM
* `200` TOO_MANY_INVALID_MSGS

## State machines (ASCII)

### Client state (per session)

```
CONNECTED
  | HELLO
  v
AUTHENTICATED (in lobby)
  | CREATE/JOIN/LIST
  v
IN_ROOM (waiting)
  | READY
  v
READY
  | both READY
  v
PLAYING
  | disconnect (short) -> DISCONNECTED (PAUSED)
  | disconnect (long)  -> REMOVED
  v
FINISHED -> IN_LOBBY
```

### Room state

```
OPEN (0..1 players)
  | players join
  v
WAITING (1..2 players)
  | both READY
  v
PLAYING
  | player DISCONNECTED (short) -> PAUSED
  | player DISCONNECTED (long) -> FINISHED
  v
FINISHED
```

## Round rules and timeouts

* bo9: first to reach 5 wins.
* Round flow:

    * Server sends `ROUND_START <n>`
    * Each player sends `MOVE <R|P|S>`
    * Server waits for both moves or `MOVE_TIMEOUT` (default 30s).
    * If a player fails to send a move within the timeout, that player LOSES the round.
    * If both send move, standard RPS rules apply; if same — DRAW (no score change).

## Reconnection policy

* `KEEPALIVE` interval: client may send `PING` every 10s.
* If server receives no data for `KEEPALIVE`(60s): marks `PLAYER_UNAVAILABLE short` and pauses game.
* `RECONNECT_WINDOW` default 120s — server keeps session state. Client attempts reconnect using `RECONNECT <token>`.
* If reconnect within window: `RECONNECT_OK` and game resumes.
* If not: server treats as long disconnect and may end game; opponent wins the match by default.

## Examples

* Client connects and registers:

    * `HELLO Bob\r\n`
    * `WELCOME afd9f3e2-...\r\n`

* Create and join room:

    * `CREATE duel\r\n` -> `ROOM_CREATED 5\r\n`
    * `JOIN 5\r\n` -> `ROOM_JOINED 5\r\n`

* Start round:

    * server: `ROUND_START 1\r\n`
    * client: `MOVE R\r\n` -> `MOVE_ACCEPTED\r\n`

---

*This document is the canonical protocol specification for the project. It includes required fields and default timeouts. Keep it in the project root as `protocol.md`.*
