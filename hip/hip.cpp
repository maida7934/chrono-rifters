#include <ncurses.h>

int main() {
    initscr();
    printw("HIP is running!");
    refresh();
    getch();
    endwin();
    return 0;
}