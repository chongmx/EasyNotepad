# EasyNotepad

A native code editor built around **reading** code, with an **AI agent as a first-class
collaborator**. C++17 with Dear ImGui; one code base for Windows, Linux, macOS, Android and the web.

> **Status: early preview.** Files, the terminal and the agent connector are real. AI summaries,
> the agent's replies and the review tools still use placeholders until their command-line
> services are connected.

## Highlights

- **Read at your level of detail**: code folds into one-line AI summaries; a logical map shows
  what the program does, down to each function's steps.
- **Work with an agent**: an Agent workspace with side-by-side diffs to accept or reject, an agent
  cursor that points and clicks for you, and an MCP connector any agent can use.
- **A complete editor**: tabs, explorer, find in files, multiple carets and VS Code's keys,
  IntelliSense, Git, and an integrated terminal running your real shell.
- **Files shown as what they are**: rendered Markdown with math and emoji, CSV tables, JSON and
  YAML trees, images, and the contents of libraries and executables.
- **Light on the machine**: it draws only when something changes, and memory is handed back when
  a folder closes.

## Get it

Download a build for your platform from
[Releases](https://github.com/chongmx/EasyNotepad/releases), or build it yourself:

```sh
git clone --recursive https://github.com/chongmx/EasyNotepad.git
scripts/build.sh --test --run     # build, run the tests, launch
```

Open a folder with `EasyNotepad <folder>` or by dropping it on the window.

## Connect an agent

Register the connector with your MCP client as a stdio server, for example Claude Code:

```sh
claude mcp add easynotepad -- "/path/to/EasyNotepad" --mcp
```

## Documentation

- [User guide](docs/GUIDE.md): every feature in detail, the MCP tools, the ribbon and colours.
- [Features](docs/FEATURES.md): feature by feature, with how each behaves at the edges.
- [Building](docs/BUILDING.md): platforms, presets, Windows 7 and 32-bit, the on-device engine.
- [Releasing](docs/RELEASING.md) and the [Changelog](CHANGELOG.md).
- [Memory](docs/MEMORY.md): how the app owns and gives back memory.
