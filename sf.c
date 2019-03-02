#include <assert.h>
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

#define SF_HIGHLIGHT_PAIR 1

typedef enum sf_entry_type_t {
  SF_ENTRY_FILE,
  SF_ENTRY_DIRECTORY,
  SF_ENTRY_LINK,
  SF_ENTRY_UNKNOWN,
} sf_entry_type_t;

typedef struct sf_entry_t {
  char name[NAME_MAX + 1];
  sf_entry_type_t type;
} sf_entry_t;

typedef struct sf_view_t {
  char path[PATH_MAX];
  uint32_t selected_entry;
  uint32_t entry_count;
  sf_entry_t *entries;
} sf_view_t;

// Path when the program was launched
char sf_initial_path[PATH_MAX];

bool sf_should_quit;

uint32_t sf_current_view;

sf_view_t sf_views[SF_VIEW_COUNT];

int sf_entry_cmp(const void *a, const void *b) {
  sf_entry_t *entry_a = (sf_entry_t *)a;
  sf_entry_t *entry_b = (sf_entry_t *)b;

  if (entry_a->type == SF_ENTRY_DIRECTORY && entry_b->type == SF_ENTRY_FILE) {
    return -1;
  }

  if (entry_a->type == SF_ENTRY_FILE && entry_b->type == SF_ENTRY_DIRECTORY) {
    return 1;
  }

  return strcoll(entry_a->name, entry_b->name);
}

void sf_get_entries(
    const char *path, uint32_t *entry_count, sf_entry_t *entries) {
  DIR *d = opendir(path);
  struct dirent *dir;
  if (d) {
    *entry_count = 0;
    while ((dir = readdir(d)) != NULL) {
      if (strcmp(dir->d_name, ".") != 0) {
        (*entry_count)++;
      }
    }

    if (entries != NULL) {
      rewinddir(d);
      uint32_t i = 0;

      while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") != 0) {
          uint32_t current = i++;
          strncpy(entries[current].name, dir->d_name, strlen(dir->d_name) + 1);

          switch (dir->d_type) {
          case DT_REG:
            entries[current].type = SF_ENTRY_FILE;
            break;
          case DT_DIR:
            entries[current].type = SF_ENTRY_DIRECTORY;
            break;
          case DT_LNK:
            entries[current].type = SF_ENTRY_LINK;
            break;
          default:
            entries[current].type = SF_ENTRY_UNKNOWN;
            break;
          }
        }
      }
    }

    closedir(d);
  }

  if (entries != NULL) {
    qsort(entries, *entry_count, sizeof(sf_entry_t), sf_entry_cmp);
  }
}

void sf_color_on(short pair) {
  if (has_colors()) {
    attron(COLOR_PAIR(pair));
  }
}

void sf_color_off(short pair) {
  if (has_colors()) {
    attroff(COLOR_PAIR(pair));
  }
}

/*
 * This call should be used by the *internal* view functions when necessary.
 */
void sf_view_update(sf_view_t *view) {
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
  view->selected_entry = 0;
  sf_view_set_path(view, sf_initial_path);
}

void sf_view_destroy(sf_view_t *view) {
  if (view->entries != NULL) {
    free(view->entries);
  }
}

/*
 * Do stuff like change the current directory to the view's path
 */
void sf_view_activate(sf_view_t *view) { chdir(view->path); }

void sf_set_view(uint32_t view_index) {
  assert(view_index >= 0 && view_index < SF_VIEW_COUNT);
  sf_current_view = view_index;

  sf_view_activate(&sf_views[sf_current_view]);
}

void sf_init() {
  sf_should_quit = false;

  getcwd(sf_initial_path, sizeof(sf_initial_path));

  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    sf_view_init(&sf_views[i]);
  }

  sf_set_view(0);

  initscr();

  raw();

  // Hide cursor
  curs_set(0);

  // TODO: handle lack of color support
  if (has_colors()) {
    start_color();

    init_pair(SF_HIGHLIGHT_PAIR, COLOR_BLUE, COLOR_BLACK);
  }
}

void sf_destroy() {
  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    sf_view_destroy(&sf_views[i]);
  }
  noraw();
  endwin();
}

void sf_draw() {
  clear();
  sf_view_t *view = &sf_views[sf_current_view];

  printw("[");
  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    if (i == sf_current_view) {
      sf_color_on(SF_HIGHLIGHT_PAIR);
    }
    printw("%d", i + 1);
    if (i == sf_current_view) {
      sf_color_off(SF_HIGHLIGHT_PAIR);
    }
    if (i + 1 < SF_VIEW_COUNT) {
      printw(" ");
    }
  }
  printw("] - %s\n", view->path);

  for (uint32_t i = 0; i < view->entry_count; i++) {
    if (i == view->selected_entry) {
      attron(A_REVERSE);
    }

    if (view->entries[i].type == SF_ENTRY_DIRECTORY) {
      sf_color_on(SF_HIGHLIGHT_PAIR);
    }

    printw("%s\n", view->entries[i].name);

    if (view->entries[i].type == SF_ENTRY_DIRECTORY) {
      sf_color_off(SF_HIGHLIGHT_PAIR);
    }

    if (i == view->selected_entry) {
      attroff(A_REVERSE);
    }
  }

  refresh();
}

int main() {
  sf_init();

  while (!sf_should_quit) {
    sf_draw();

    sf_view_t *view = &sf_views[sf_current_view];

    char c;
    switch (c = getch()) {
    case 'h': {
      sf_view_set_path(view, "..");
      // TODO: use a stack to keep track of past selected entries
      view->selected_entry = 0;
      break;
    }
    case 'l': {
      sf_entry_t *entry = &view->entries[view->selected_entry];
      if (entry->type == SF_ENTRY_DIRECTORY) {
        char path[PATH_MAX];
        realpath(entry->name, path);
        sf_view_set_path(view, path);
        // TODO: use a stack to keep track of past selected entries
        view->selected_entry = 0;
      } else if (entry->type == SF_ENTRY_FILE) {
        // TODO: handle opening files
      } else {
        // TODO: handle links and stuff
      }
      break;
    }
    case 'j': {
      // Move down
      if (view->selected_entry < view->entry_count - 1) {
        view->selected_entry++;
      }
      break;
    }
    case 'k': {
      // Move up
      if (view->selected_entry > 0) {
        view->selected_entry--;
      }
      break;
    }
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      uint32_t view_index = (uint32_t)(c - '1');
      if (view_index >= 0 && view_index < SF_VIEW_COUNT) {
        sf_set_view(view_index);
      }
      break;
    }
    case 'q':
      sf_should_quit = true;
      break;
    }
  }

  sf_destroy();

  return 0;
}
