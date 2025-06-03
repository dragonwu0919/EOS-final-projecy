#include <ncurses.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>    
#include <stdio.h>
#include <syslog.h>
#include <poll.h>
#include "order_comm.h" 

#define MAX_TASKS 64
#define NUM_CHEFS  3
#define MAX_PLATES 3

void* kitchen_event_thread(void *arg);
void* chef_thread(void *arg);

int move_delay_us = 50000;
pthread_mutex_t screen_lock = PTHREAD_MUTEX_INITIALIZER;
int chef_positions[NUM_CHEFS][2] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 }   /* 廚師的初始位置 */
};

/* 想去的位置 */
int chef_desired_pos[NUM_CHEFS][2] = {
    { -1, -1 },
    { -1, -1 },
    { -1, -1 }   /* 廚師的目標位置 */
};

void init_scaling() {
    int term_w, term_h;
    getmaxyx(stdscr, term_h, term_w);
    if (term_w < 100 || term_h < 30) {
        endwin();
        printf("[DEBUG] Terminal too small: current = %d x %d (need ≥ 100 x 30)\n",
               term_w, term_h);
        exit(1);
    } else {
        printf("[DEBUG] Term	inal size OK: %d x %d\n", term_w, term_h);
    }
}


struct { int top, left, h, w; } boxes[] = {
    { 1,   5,   7,  35},// 碗盤回收區
    { 1,  45,   5,  35},// 裝盤區
    { 1,  90,  33,  25},// 洗手槽
    {10,   5,  15,  10},// 切菜區
    {27,   5,   7,  35},// 材料區
    {29,  45,   5,  35},// 爐台區
};
static int nboxes = sizeof(boxes) / sizeof(boxes[0]);

int is_blocked(int x, int y) {
    for (int i = 0; i < nboxes; i++) {
        int t = boxes[i].top , l = boxes[i].left ;
        int h = boxes[i].h , w = boxes[i].w ;
        if (x >= l && x < l + w && y >= t && y < t + h) return 1;
    }
    return 0;
}

void clamp_target(int *tx, int *ty) {
    for (int i = 0; i < nboxes; i++) {
        int t = boxes[i].top , l = boxes[i].left ;
        int h = boxes[i].h , w = boxes[i].w ;
        if (*tx >= l && *tx < l + w && *ty >= t && *ty < t + h) {
            int da = *ty - t, db = (t + h - 1) - *ty;
            int dl = *tx - l, dr = (l + w - 1) - *tx;
            int m = da; char dir = 'A';
            if (db < m) { m = db; dir = 'B'; }
            if (dl < m) { m = dl; dir = 'L'; }
            if (dr < m) { m = dr; dir = 'R'; }
            if (dir == 'A') *ty = t - 1;
            if (dir == 'B') *ty = t + h;
            if (dir == 'L') *tx = l - 1;
            if (dir == 'R') *tx = l + w;
            return;
        }
    }
}

void draw_ascii_box(int top, int left, int height, int width,
                    const char* title, const char* subtitle) {
    int t = top ;
    int l = left ;
    int h = height ;
    int w = width ;

    mvprintw(t, l, "+");
    for (int x = 1; x < w - 1; x++) addch('-');
    addch('+');
    for (int y = 1; y < h - 1; y++) {
        mvprintw(t + y, l, "|");
        mvprintw(t + y, l + w - 1, "|");
    }
    mvprintw(t + h - 1, l, "+");
    for (int x = 1; x < w - 1; x++) addch('-');
    addch('+');
}

void draw_layout() {
    clear();
    draw_ascii_box( 1,   5,   7,  35, "", "");
    draw_ascii_box( 1,  45,   5,  35, "", "");
    draw_ascii_box( 1,  90,  33,  25, "", "");
    draw_ascii_box(10,   5,  15,  10, "", "");
    draw_ascii_box(27,   5,   7,  35, "", "");
    draw_ascii_box(29,  45,   5,  35, "", "");
 

    struct {
        const char *text; int x, y;
    } labels[] = {
        {"Return Area", 6, 2},
        {"1 space", 6, 3},
        {"Plating Area", 91, 2},
        {"3 spaces", 91, 3},
        {"Sink", 46, 2},
        {"1 workspace", 46, 3},
        {"Cut", 6, 11},
        {"2 spaces", 6, 12},
        {"Refrigerator", 6, 28},
        {"1 space", 6, 29},
        {"Stove", 46, 31},
        {"1 space", 46, 32},
    };

    for (int i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
        mvprintw(labels[i].y , labels[i].x , "%s", labels[i].text);

    refresh();
}

void draw_chef(int cx, int cy, const char* chef_id) {
    attron(A_BOLD);
    mvprintw(cy, cx, "%s", chef_id);
    attroff(A_BOLD);
}

int can_move_to(int nx, int ny, int self_index) {
    int my_x = chef_positions[self_index][0];
    int my_y = chef_positions[self_index][1];

    for (int i = 0; i < NUM_CHEFS; i++) {
        if (i == self_index) continue;

        int ox = chef_positions[i][0];
        int oy = chef_positions[i][1];
        int odx = chef_desired_pos[i][0];
        int ody = chef_desired_pos[i][1];

        // // 🔴 雙向對調偵測（第一優先判斷）
        // if (ox == nx && oy == ny &&
        //     odx == my_x && ody == my_y) {
        //     // 兩人要交換位置
        //     if (i < self_index)
        //         return 0; // 自己優先權低就讓
        //     else
        //         continue; // 自己優先就通過
        // }

        // // 🔴 對方已經在我要的位置
        // if (ox == nx && oy == ny)
        //     return 0;

        // // 🔴 對方也想去我想去的格，且他優先
        // if (odx == nx && ody == ny && i < self_index)
        //     return 0;
    }
    return 1;
}

void move_chef(int *cx, int *cy, int tx, int ty,
               const char* chef_id, int self_index)
{
    int prev_x = *cx, prev_y = *cy;
    clamp_target(&tx, &ty);

    while (*cx != tx || *cy != ty) {
        int dx = (tx > *cx) ? 1 : (tx < *cx ? -1 : 0);
        int dy = (ty > *cy) ? 1 : (ty < *cy ? -1 : 0);
        int nx = *cx, ny = *cy;

        // 嘗試往 X 方向
        if (dx && !is_blocked(*cx + dx, *cy))
            nx = *cx + dx;

        // 嘗試往 Y 方向（如果還沒動）
        if (nx == *cx && dy && !is_blocked(*cx, *cy + dy))
            ny = *cy + dy;

        // 嘗試斜向移動（如果還沒動）
        if (nx == *cx && ny == *cy && dx && dy && !is_blocked(*cx + dx, *cy + dy)) {
            nx = *cx + dx;
            ny = *cy + dy;
        }

        // 嘗試斜繞左下或右上（Z字型）
        if (nx == *cx && ny == *cy && dx && dy && !is_blocked(*cx + dx, *cy - dy)) {
            nx = *cx + dx;
            ny = *cy - dy;
        }
        if (nx == *cx && ny == *cy && dx && dy && !is_blocked(*cx - dx, *cy + dy)) {
            nx = *cx - dx;
            ny = *cy + dy;
        }

        // 若還是卡住，就停一拍（不強行推進）
        if (nx == *cx && ny == *cy) {
            usleep(move_delay_us);
            continue;
        }

        // 更新欲前往位置
        chef_desired_pos[self_index][0] = nx;
        chef_desired_pos[self_index][1] = ny;

        // 同步避障：其他廚師也想去，就讓步
        // if (!can_move_to(nx, ny, self_index)) {
        //     usleep(move_delay_us);
        //     continue;
        // }

        // ✅ 正式移動
        *cx = nx;
        *cy = ny;
        chef_positions[self_index][0] = nx;
        chef_positions[self_index][1] = ny;

        pthread_mutex_lock(&screen_lock);
        // 清除舊位置
        mvprintw(prev_y, prev_x, "  ");
        // 畫新位置
        attron(A_BOLD);
        mvprintw(ny, nx, "%s", chef_id);
        attroff(A_BOLD);
        refresh();
        pthread_mutex_unlock(&screen_lock);

        prev_x = nx;
        prev_y = ny;
        mvprintw(1 + self_index, 1,
         "Chef %s reached (%2d,%2d)", chef_id, tx, ty);
        usleep(move_delay_us);
    }

    // —— 到達 (tx, ty) 之後的回呼/日誌 —— 
    pthread_mutex_lock(&screen_lock);
    mvprintw( 1 + self_index, 1,
             "Chef %s reached (%2d,%2d)    ",
             chef_id, tx, ty);
    refresh();
    pthread_mutex_unlock(&screen_lock);
}

void* chef_thread(void *arg) {
     int fd = *(int*)arg;
     char buf[128];
     while (1) {
         int n = read(fd, buf, sizeof(buf)-1);
         if (n > 0) {
             buf[n] = '\0';
             int id, order_id;
             char type[16], menu[32];
             // 解析 "id:TYPE:order:menu"
             if (sscanf(buf, "%d:%15[^:]:%d:%31[^\n]", &id, type, &order_id, menu) >= 3) {
                 int info_y = 45 + id * 5;
                 pthread_mutex_lock(&screen_lock);
                 if (strcmp(type, "START") == 0) {
                     mvprintw(info_y, 5,
                              "Chef %d start order %d [%s]    ",
                              id+1, order_id, menu);
                 } else {
                     mvprintw(info_y, 5,
                              "Chef %d done  order %d         ",
                              id+1, order_id);
                 }
                 refresh();
                 pthread_mutex_unlock(&screen_lock);
             }
         } else {
             // 無新事件就稍等
             usleep(100000);
         }
     }
     return NULL;
 }

void* chef_task_thread(void *arg) {
    int task_fd = *(int*)arg;
    char task_buf[128];

    while (1) {
        int n = read(task_fd, task_buf, sizeof(task_buf) - 1);
        if (n > 0) {
            task_buf[n] = '\0';

            int order_id, dish_id, step, x, y;
            if (sscanf(task_buf, "%d %d %d %d %d", &order_id, &dish_id, &step, &x, &y) == 5) {
                // 根據 order_id 決定 chef_id（簡單分配）
                int chef_id = order_id % 3;

                pthread_mutex_lock(&screen_lock);
                mvprintw(20, 5, "Chef Task: order %d, dish %d, step %d at (%d,%d)   ",
                         order_id, dish_id, step, x, y);
                refresh();
                pthread_mutex_unlock(&screen_lock);

                //printf("[GUI] move_chef: chef %d -> (%d,%d)\n", chef_id, x, y);

                move_chef(&chef_positions[chef_id][0],
                          &chef_positions[chef_id][1],
                          x, y,
                          chef_id == 0 ? "A" : (chef_id == 1 ? "B" : "C"),
                          chef_id);

                // ✅ ACK 由 kitchen_event_thread 負責，不在這邊寫
            } else {
                printf("[GUI] Failed to parse task_buf: \"%s\"\n", task_buf);
            }
        } else {
            usleep(100000);  // 沒任務就稍作等待
        }
    }

    return NULL;
}


int main() {
    /* ncurses 初始化 */
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    init_scaling();
    int task_fd = open("/dev/chef_tasks", O_RDWR | O_NONBLOCK);
    int kitchen_fd = open("/dev/kitchen", O_RDONLY );
    if (kitchen_fd < 0) {
        perror("open /dev/kitchen failed");
        endwin();
        return 1;
    }	

    /* 畫出背景格局 */
    draw_layout();

    /* 設定三位廚師的初始位置 */
    chef_positions[0][0] = 45;  chef_positions[0][1] = 12;
    chef_positions[1][0] = 50;  chef_positions[1][1] = 12;
    chef_positions[2][0] = 55;  chef_positions[2][1] = 12;

    /* 把三位廚師畫在初始位置 */
    draw_chef(chef_positions[0][0], chef_positions[0][1], "1");
    draw_chef(chef_positions[1][0], chef_positions[1][1], "2");
    draw_chef(chef_positions[2][0], chef_positions[2][1], "3");
    refresh();

    /* 打開 /dev/kitchen 以非阻塞模式讀事件 */

    if (kitchen_fd < 0) {
        perror("open /dev/kitchen");
        endwin();
        return 1;
    }

    /* 啟動監聽 kernel 事件的執行緒 */
    pthread_t kitchen_tid;
    pthread_create(&kitchen_tid, NULL, kitchen_event_thread, &kitchen_fd);
    int ret = pthread_create(&kitchen_tid, NULL, kitchen_event_thread, &kitchen_fd);
    if (ret != 0) {
    	perror("pthread_create kitchen_event_thread failed");
    return 1;
}
    printf("Chef task thread started!\n");

    /* 等候事件執行緒結束（通常不會)] */
    pthread_join(kitchen_tid, NULL);
    
    close(task_fd);

    /* 按任意鍵退出 */
    getch();
    endwin();
    return 0;
}

void* kitchen_event_thread(void *arg) {
    int fd = *(int*)arg;
    char buf[128];

    //printf("[GUI] kitchen_event_thread started\n");
    
    int task_fd, kitchen_fd_w;
    
    task_fd = open("/dev/chef_tasks", O_RDWR | O_NONBLOCK);
    if (task_fd < 0) {
        perror("open /dev/chef_tasks failed");
        pthread_exit(NULL);
    }
    
    kitchen_fd_w = open("/dev/kitchen", O_WRONLY | O_NONBLOCK);
    if (kitchen_fd_w < 0) {
        perror("open /dev/kitchen for write failed");
        pthread_exit(NULL);
    }
    while (1) {
    
        // --- poll kitchen_fd ---
        struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
    };

    //printf("[GUI] poll start\n");

    int ret = poll(&pfd, 1, 100); // 最多等 100ms

    if (ret < 0) {
        perror("poll failed");
        break;
    } else if (ret == 0) {
        printf("[GUI] poll timeout (no event)\n");
    } else {
        printf("[GUI] poll triggered: revents = 0x%x\n", pfd.revents);
        // 確保是 POLLIN 被觸發
        if (pfd.revents & POLLIN) {
            struct order_event ev;
            int n = read(fd, &ev, sizeof(ev));
            if (n < 0) {
                perror("read kitchen_fd failed");
            } else if (n == 0) {
                printf("[GUI] kitchen_fd read returned 0\n");
            } else if (n < sizeof(ev)) {
                printf("[GUI] kitchen_fd read partial: %d bytes\n", n);
            } else {
                printf("[GUI] recv ev: chef=%d step=%d pos=(%d,%d)\n",
                       ev.chef_id, ev.step_idx, ev.x, ev.y);
                refresh();
                int id = ev.chef_id;

                move_chef(&chef_positions[id][0], &chef_positions[id][1],
                          ev.x, ev.y,
                          id == 0 ? "1" : (id == 1 ? "2" : "3"), id);

                struct order_ack ack = {
                    .chef_id = ev.chef_id,
                    .step_idx = ev.step_idx
                };
                size_t w = write(kitchen_fd_w, &ack, sizeof(ack));
                if (w == sizeof(ack)) {
                    printf("[GUI] ACK success: chef=%d step=%d\n", ev.chef_id, ev.step_idx);
                } else {
                    printf("[GUI] ACK failed: write returned %ld\n", w);
                }
            }
        }

        // --- 任務顯示部分（維持不動） ---
        char task_buf[128];
        int m = read(task_fd, task_buf, sizeof(task_buf) - 1);
        if (m > 0) {
            task_buf[m] = '\0';

            int order_id, dish_id, step, x, y;
            if (sscanf(task_buf, "%d %d %d %d %d", &order_id, &dish_id, &step, &x, &y) == 5) {
                int id = order_id % 3;

                pthread_mutex_lock(&screen_lock);
                mvprintw(20, 5, "Chef Task: order %d, dish %d, step %d at (%d,%d)   ",
                         order_id, dish_id, step, x, y);
                refresh();
                pthread_mutex_unlock(&screen_lock);

                //printf("[GUI] move_chef: chef %d -> (%d,%d)\n", id, x, y);
            } else {
                //printf("[GUI] Failed to parse task_buf: \"%s\"\n", task_buf);
            }
        }

        usleep(100000);
    }

    close(task_fd);
    close(kitchen_fd_w);
    return NULL;
  }
}






