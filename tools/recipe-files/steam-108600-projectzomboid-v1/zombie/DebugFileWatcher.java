// SPDX-License-Identifier: GPL-3.0-or-later
// Stands in for Project Zomboid's folder watcher, which holds a handle on each of the game's 1704 media folders: more
// than a PS5 title can hold open. It only reloads files changed while the game runs. This one watches nothing.
package zombie;

public final class DebugFileWatcher {
    public static final DebugFileWatcher instance = new DebugFileWatcher();
    private boolean initialized;

    private DebugFileWatcher() {
    }

    public void init() {
        initialized = true;
    }

    public boolean isInitialized() {
        return initialized;
    }

    public void add(PredicatedFileWatcher watcher) {
    }

    public void addDirectory(String path) {
    }

    public void addDirectoryRecurse(String path) {
    }

    public void remove(PredicatedFileWatcher watcher) {
    }

    public void update() {
    }
}
