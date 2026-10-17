#pragma once
#include <string>
#include <vector>

// The sync agent (docs/sync-plan.md, "Architecture"): `explorer-plus.exe --sync` runs the
// pairs of this settings folder in the background, one engine per pair, with a tray icon
// (`--headless`: none, for tests) and the IPC pipe Explorer Plus and the CLI talk to.
// Single instance per settings folder.
int sync_agent_main(bool headless);

// `explorer-plus.exe --sync-cli <command> [key=value ...]`: one request to the agent, the
// reply printed as key=value lines (exit code 0: ok, 1: refused, 2: no agent). Local
// commands that need no agent: add-pair, remove-pair, start-agent, pairs-file, forget-keys.
int sync_cli_main(const std::vector<std::wstring>& args);

// starts the agent when there are pairs and it isn't running (Explorer Plus at launch)
bool sync_agent_ensure_running();
// starts it whether or not there are pairs (Sync folders, before the first one); waits for its pipe
bool sync_agent_start();
