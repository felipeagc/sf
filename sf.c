#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <ncurses.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SF_VIEW_COUNT 4

#define SF_HIGHLIGHT_PAIR 1
#define SF_EMPTY_PAIR 2

#define SF_OPENER "xdg-open"
#define SF_EDITOR "nvim"

/*
 * sf_spawn flags
 */
#define SF_FLAG_NOTRACE 1 << 0 // Disable child output
#define SF_FLAG_TERM 1 << 1    // Exit curses white process is running
#define SF_FLAG_NOWAIT 1 << 2  // Don't wait for the child process to exit

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
  uint32_t *offset_stack;
  uint32_t stack_size;
} sf_view_t;

typedef struct sf_pane_t {
  WINDOW *window;
} sf_pane_t;

// Path when the program was launched
char sf_initial_path[PATH_MAX];

bool sf_should_quit;

uint32_t sf_current_view;

sf_view_t sf_views[SF_VIEW_COUNT];

sf_pane_t sf_main_pane;

/*
 * argv must be either NULL or a NULL terminated array
 */
void sf_spawn(char *const argv[], uint32_t flags) {
  if (flags & SF_FLAG_TERM) {
    endwin();
  }

  pid_t pid = fork();
  if (pid == 0) {
    // Disable child output
    if (flags & SF_FLAG_NOTRACE) {
      int fd = open("/dev/null", O_WRONLY, 0200);

      dup2(fd, 1); // stdout
      dup2(fd, 2); // stderr
      close(fd);
    }

    if (flags & SF_FLAG_NOWAIT) {
      signal(SIGHUP, SIG_IGN);
      signal(SIGPIPE, SIG_IGN);
      setsid();
    }

    execvp(argv[0], argv);
    _exit(1);
  } else {
    if (!(flags & SF_FLAG_NOWAIT))
      /* Ignore interruptions */
      while (waitpid(pid, NULL, 0) == -1) {
      }
    if (flags & SF_FLAG_TERM) {
      refresh();
    }
  }
}

uint32_t sf_get_path_level(const char *path) {
  char rpath[PATH_MAX];
  realpath(path, rpath);

  assert(strlen(rpath) >= 1);

  if (strlen(rpath) == 1) {
    return 0;
  }

  uint32_t level = 0;

  for (uint32_t i = 0; i < strlen(rpath); i++) {
    if (rpath[i] == '/') {
      level++;
    }
  }

  return level;
}

void sf_get_top_dir_from_path(const char *path, char *dest) {
  assert(strlen(path) > 0);

  if (strlen(path) == 1 && strcmp(path, "/") == 0) {
    strncpy(dest, path, strlen(path) + 1);
    return;
  }

  uint32_t slash_index = 0;
  for (uint32_t i = strlen(path) - 1; i >= 0; i--) {
    if (path[i] == '/') {
      slash_index = i;
      break;
    }
  }

  strncpy(dest, &path[slash_index + 1], strlen(path) - slash_index);
}

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
      if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
        (*entry_count)++;
      }
    }

    if (entries != NULL) {
      rewinddir(d);
      uint32_t i = 0;

      while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
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

void sf_pcolor_on(sf_pane_t *pane, short pair) {
  if (has_colors()) {
    wattron(pane->window, COLOR_PAIR(pair));
  }
}

void sf_pcolor_off(sf_pane_t *pane, short pair) {
  if (has_colors()) {
    wattroff(pane->window, COLOR_PAIR(pair));
  }
}

void sf_view_update_stacks(sf_view_t *view) {
  uint32_t old_stack_size = view->stack_size;
  view->stack_size = sf_get_path_level(view->path) + 2;

  if (old_stack_size < view->stack_size) {
    view->offset_stack =
        realloc(view->offset_stack, view->stack_size * sizeof(uint32_t));

    for (uint32_t i = old_stack_size; i < view->stack_size; i++) {
      view->offset_stack[i] = 0;
    }
  }
}

void sf_view_set_selected_entry(sf_view_t *view, uint32_t entry_index) {
  view->selected_entry = entry_index;

  if (entry_index >= view->entry_count) {
    entry_index = view->entry_count - 1;
  }

  if (entry_index < 0) {
    entry_index = 0;
  }

  sf_view_update_stacks(view);

  uint32_t level = sf_get_path_level(view->path);
  assert(level < view->stack_size);

  // When you change the current selected directory, reset the next one in
  // the stack
  assert((level + 1) < view->stack_size);
  view->offset_stack[level + 1] = 0;
}

void sf_view_update_entries(sf_view_t *view) {
  sf_get_entries(".", &view->entry_count, NULL);
  view->entries =
      realloc(view->entries, sizeof(sf_entry_t) * view->entry_count);
  sf_get_entries(".", &view->entry_count, view->entries);

  if (view->entry_count <= 1) {
    sf_view_set_selected_entry(view, 0);
  }
}

void sf_view_set_path(sf_view_t *view, const char *path) {
  char rpath[PATH_MAX];
  realpath(path, rpath);
  if (chdir(rpath) == 0) {
    // Success
    strncpy(view->path, rpath, strlen(rpath) + 1);
    chdir(sf_views[sf_current_view].path);
    sf_view_update_entries(view);
    sf_view_update_stacks(view);
  }
}

void sf_view_init(sf_view_t *view) {
  view->entry_count = 0;
  view->entries = NULL;
  view->selected_entry = 0;
  sf_view_set_path(view, sf_initial_path);

  view->stack_size = sf_get_path_level(view->path) + 2;
  view->offset_stack = calloc(view->stack_size, sizeof(uint32_t));

  for (uint32_t i = 0; i < view->stack_size; i++) {
    view->offset_stack[i] = 0;
  }
}

void sf_view_destroy(sf_view_t *view) {
  if (view->entries != NULL) {
    free(view->entries);
  }

  free(view->offset_stack);
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

void sf_pane_init(sf_pane_t *pane, int height, int width, int y, int x) {
  pane->window = newwin(height, width, y, x);
}

void sf_pane_destroy(sf_pane_t *pane) { delwin(pane->window); }

void sf_init() {
  sf_should_quit = false;

  getcwd(sf_initial_path, sizeof(sf_initial_path));

  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    sf_view_init(&sf_views[i]);
  }

  sf_set_view(0);

  initscr();

  cbreak();
  raw();

  // Hide cursor
  curs_set(0);

  // TODO: handle lack of color support
  if (has_colors()) {
    start_color();

    init_pair(SF_HIGHLIGHT_PAIR, COLOR_BLUE, COLOR_BLACK);
    init_pair(SF_EMPTY_PAIR, COLOR_WHITE, COLOR_RED);
  }

  refresh();

  sf_pane_init(&sf_main_pane, LINES, COLS, 0, 0);
}

void sf_destroy() {
  for (uint32_t i = 0; i < SF_VIEW_COUNT; i++) {
    sf_view_destroy(&sf_views[i]);
  }
  sf_pane_destroy(&sf_main_pane);
  noraw();
  endwin();
}

void sf_draw_header() {
  sf_view_t *view = &sf_views[sf_current_view];
  move(0, 0);
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
  printw("] - %s", view->path);
}

void sf_draw_pane(sf_pane_t *pane) {
  wclear(pane->window);
  sf_view_t *view = &sf_views[sf_current_view];

  int width, height;
  getmaxyx(pane->window, height, width);

  if (view->entry_count == 0) {
    sf_pcolor_on(pane, SF_EMPTY_PAIR);
    mvwprintw(pane->window, 1, 0, "empty");
    sf_pcolor_on(pane, SF_EMPTY_PAIR);
  } else {
    uint32_t level = sf_get_path_level(view->path);
    assert(level < view->stack_size);
    uint32_t *offset = &view->offset_stack[level];

    if (view->selected_entry < *offset) {
      *offset = view->selected_entry;
    }

    if (view->selected_entry >= *offset + height - 1) {
      *offset = view->selected_entry - height + 2;
    }
    for (uint32_t i = *offset; i < view->entry_count; i++) {
      if (i == view->selected_entry) {
        wattron(pane->window, A_REVERSE);
      }

      if (view->entries[i].type == SF_ENTRY_DIRECTORY) {
        sf_pcolor_on(pane, SF_HIGHLIGHT_PAIR);
      }

      int y = i - *offset + 1;

      mvwprintw(pane->window, y, 0, "%s", view->entries[i].name);

      if (i == view->selected_entry) {
        uint32_t spaces = width - strlen(view->entries[i].name);
        for (uint32_t j = 0; j < spaces; j++) {
          mvwprintw(pane->window, y, strlen(view->entries[i].name) + j, " ");
        }
      }

      if (view->entries[i].type == SF_ENTRY_DIRECTORY) {
        sf_pcolor_off(pane, SF_HIGHLIGHT_PAIR);
      }

      if (i == view->selected_entry) {
        wattroff(pane->window, A_REVERSE);
      }
    }
  }

  wrefresh(pane->window);
}

int main() {
  sf_init();

  while (!sf_should_quit) {
    sf_draw_header();
    sf_draw_pane(&sf_main_pane);

    sf_view_t *view = &sf_views[sf_current_view];

    int c;
    switch (c = getch()) {
    case 'h': {
      // Go back a directory
      char prev_name[NAME_MAX];
      sf_get_top_dir_from_path(view->path, prev_name);
      sf_view_set_path(view, "..");

      for (uint32_t i = 0; i < view->entry_count; i++) {
        if (strcmp(prev_name, view->entries[i].name) == 0) {
          sf_view_set_selected_entry(view, i);
        }
      }

      break;
    }
    case 'l': {
      sf_entry_t *entry = &view->entries[view->selected_entry];
      if (entry->type == SF_ENTRY_DIRECTORY) {
        // Go into directory
        char path[PATH_MAX];
        realpath(entry->name, path);
        sf_view_set_path(view, path);

        assert(sf_get_path_level(view->path) < view->stack_size);
        sf_view_set_selected_entry(view, 0);
      } else {
        // TODO: handle links and stuff
      }
      break;
    }
    case 'j': {
      // Move down
      if (view->selected_entry < view->entry_count - 1) {
        sf_view_set_selected_entry(view, view->selected_entry + 1);
      }
      break;
    }
    case 'k': {
      // Move up
      if (view->selected_entry > 0) {
        sf_view_set_selected_entry(view, view->selected_entry - 1);
      }
      break;
    }
    case '\n': {
      // Open file
      sf_entry_t *entry = &view->entries[view->selected_entry];
      if (entry->type == SF_ENTRY_FILE) {
        char path[PATH_MAX];
        realpath(entry->name, path);

        char *const args[] = {SF_OPENER, path, NULL};
        sf_spawn(args, SF_FLAG_NOTRACE | SF_FLAG_NOWAIT);
      }
      break;
    }
    case 'e': {
      sf_entry_t *entry = &view->entries[view->selected_entry];

      char path[PATH_MAX];
      realpath(entry->name, path);

      char *const args[] = {SF_EDITOR, path, NULL};
      sf_spawn(args, SF_FLAG_TERM);
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
    case 'q': {
      sf_should_quit = true;
      break;
    }
    case KEY_RESIZE: {
      clear();
      refresh();
      break;
    }
    }
  }

  sf_destroy();

  return 0;
}
