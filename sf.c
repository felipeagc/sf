#include <dirent.h>
#include <limits.h>
#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SF_VIEW_COUNT 4

typedef struct sf_entry_t {
  char *name;
} sf_entry_t;

typedef struct sf_view_t {
  char path[PATH_MAX];
  uint32_t entry_count;
  sf_entry_t *entries;
} sf_view_t;

// Path when the program was launched
char sf_initial_path[PATH_MAX];

bool sf_should_quit;

uint32_t sf_current_view;

sf_view_t sf_views[SF_VIEW_COUNT];

void sf_get_entries(
    const char *path, uint32_t *entry_count, sf_entry_t *entries) {
  DIR *d = opendir(path);
  struct dirent *dir;
  if (d) {
    *entry_count = 0;
    while ((dir = readdir(d)) != NULL) {
      (*entry_count)++;
    }

    if (entries != NULL) {
      rewinddir(d);
      uint32_t i = 0;

      while ((dir = readdir(d)) != NULL) {
        uint32_t current = i++;
        entries[current].name =
            malloc(sizeof(char) * (strlen(dir->d_name) + 1));
        strncpy(entries[current].name, dir->d_name, strlen(dir->d_name) + 1);
      }
    }

    closedir(d);
  }
}

void sf_entry_destroy(sf_entry_t *entry) {
  if (entry->name != NULL) {
    free(entry->name);
  }
}

/*
 * This call should be used by the *internal* view functions when necessary.
 */
void sf_view_update(sf_view_t *view) {
  for (uint32_t i = 0; i < view->entry_count; i++) {
    sf_entry_destroy(&view->entries[i]);
  }
  if (view->entries != NULL) {
    free(view->entries);
  }

  sf_get_entries(".", &view->entry_count, NULL);
  view->entries = malloc(sizeof(sf_entry_t) * view->entry_count);
  sf_get_entries(".", &view->entry_count, view->entries);
}

void sf_view_set_path(sf_view_t *view, const char *path) {
  chdir(view->path);
  realpath(path, view->path);
  chdir(sf_views[sf_current_view].path);
  sf_view_update(view);
}

void sf_view_init(sf_view_t *view) {
  view->entry_count = 0;
  view->entries = NULL;
  sf_view_set_path(view, sf_initial_path);
}

void sf_view_destroy(sf_view_t *view) {
  for (uint32_t i = 0; i < view->entry_count; i++) {
    sf_entry_destroy(&view->entries[i]);
  }
  if (view->entries != NULL) {
    free(view->entries);
  }
}

/*
 * Do stuff like change the current directory to the view's path
 */
void sf_view_activate(sf_view_t *view) { chdir(view->path); }

void sf_init() {
  sf_should_quit = false;

  getcwd(sf_initial_path, sizeof(sf_initial_path));

  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    sf_view_init(&sf_views[i]);
  }

  sf_current_view = 0;
  sf_view_activate(&sf_views[sf_current_view]);

  initscr();
}

void sf_destroy() {
  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    sf_view_destroy(&sf_views[i]);
  }
  endwin();
}

void sf_draw() {
  clear();

  sf_view_t *view = &sf_views[sf_current_view];

  printw("Current view: %d\n", sf_current_view);
  printw("Path: %s\n", view->path);

  for (uint32_t i = 0; i < view->entry_count; i++) {
    printw("%s\n", view->entries[i]);
  }

  refresh();
}

int main() {
  sf_init();

  while (!sf_should_quit) {
    sf_draw();

    switch (getch()) {
    case 'h':
      sf_view_set_path(&sf_views[sf_current_view], "..");
      break;
    default:
      sf_should_quit = true;
      break;
    }
  }

  sf_destroy();

  return 0;
}
