# SPN Relayserver

**IMPORTANT**: This is a repository related to the GPN18 version of SPN. It is
no longer maintained. See https://github.com/schlangenprogrammiernacht/spn-meta
for the GPN19 version!

This repository contains the relayserver program, which distributes game data to multiple clients. This has two main purposes: convert the stream from raw MessagePack data to a websocket and remove that load from the gameserver.

If a client blocks or the relayserver crashes, the gameserver is unaffected.
