---
name: meetings
description: transcribe recordings and meetings into a text file, then write minutes and action items
---

# Meetings and recordings

## Transcribe
`transcribe {"path": "audio/meeting.wav"}` - only the path. It writes down every word said, with
[mm:ss] times, into `audio/meeting.transcript.md` (next to the recording; an existing file is never
overwritten), opens it for the user, and gives you the text. WAV, MP3 and FLAC; any length (it takes
30 seconds at a time, so a word at a boundary can be cut). Summaries, minutes and translations you
write yourself afterwards, from that text.
- A short clip to answer a question about: `listen_to` is quicker.

## Then, if asked: minutes
From the transcript, write:
1. **Summary** - three to five sentences.
2. **Decisions** - what was agreed.
3. **Action items** - who does what by when ("Lisa: confirm the migration with Priya, by Friday").
4. **Open questions**.
Propose them as a new file with `write_file` (e.g. `audio/meeting.minutes.md`) when the user wants
them kept. Do not invent names, dates or numbers that were not said; mark unclear words [unclear].

## People
A transcript does not say who speaks. Use the names that are said ("Thanks, Mark" - the next
speaker talks to Mark); "I" is the speaker, whose name you may not know: write "the chair" or
"a participant", never "you" or "the agent". List every action item that was said, with its owner
and date, even the last ones.
