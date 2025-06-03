// order_comm.h
#ifndef ORDER_COMM_H
#define ORDER_COMM_H

struct order {
    int order_id;
    int menu_idx;
    char name[32];
};

struct order_event {
    int chef_id;      // 第幾號廚師
    int step_idx;     // 這是該菜的第幾步
    int x, y;         // GUI 上要移動的座標
    int pause_sec;    // 到達後要停留的秒數
};

/* user → kernel */
struct __attribute__((packed))order_ack {
    int chef_id;
    int step_idx;     // 與上面對應，表示該步驟已完成
};

/* 將訂單推入佇列，成功回傳0，佇列滿回傳-ENOSPC */
int add_order(struct order *o);

/* 從佇列取出一筆訂單，若無訂單則阻塞 */
struct order *recv_order(void);

static const char *menu_names[] = {
    "Cucumber roll", "Sashimi", "Fish roll", "Cucumber Salad", "Tomato Salad",
    "Tomato Cucumber Salad", "Green Tea", "Black Tea", "Oolong Tea"
};

#endif // ORDER_COMM_H

