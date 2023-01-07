/*
 *    Project 3 Channels in C language 
 *          tgroup88: Bert Yan 
 * 
 *    Lab machine grade:      182/200
 *    Personal machine grade: 181/200
 *    Need more work...
 *    
 *    Oddly the timeout test is an issue on personal pc but not at all if ran on lab machine
 */

#include <stdint.h>
#include <assert.h>
#include "channel.h"

/**
 * Debugging Utilities
 */
// #define DEBUG

#ifdef DEBUG
#define DEBUG_EXPR(expr) expr
#else
#define DEBUG_EXPR(expr)
#endif

/*
 * print statement with thread number, to check some race conditions
 */
#define THREAD_PRINT(...)  do{\
    printf("%lu: ", pthread_self());\
    printf(__VA_ARGS__);      \
    printf("\n"); \
} while(0)

// lock channel
#define CHANNEL_LOCK(ch) do {if (pthread_mutex_lock(&(ch)->chan_lck))  return OTHER_ERROR; }while(0)

// unlock and return, frequenly used
#define UNLOCK_RETURN(lck, r) do{pthread_mutex_unlock(lck); return (r);}while(0)

// enter channel as a reader
// will increase the reader count
enum chan_status channel_validate(chan_t* ch) {
    CHANNEL_LOCK(ch);
    if (ch->closed) UNLOCK_RETURN(&ch->chan_lck, CLOSED_ERROR);
    ch->ref_cnt += 1;
    return SUCCESS;
}

// handy code for code like
// pthread_mutex_lock(ch->lock),
// if (channle->closed) pthread_mutex_unlock(ch->lock)
#define ENTER_CHANNEL(ch) do {\
   enum chan_status ENTER_CHANNEL_stat = channel_validate(ch); \
   if (ENTER_CHANNEL_stat != SUCCESS) return ENTER_CHANNEL_stat; } while(0)


#define LEAVE_CHANNEL(ch, status) do {\
    (ch)->ref_cnt -= 1;                  \
    if (((ch)->ref_cnt) == 0) {       \
        pthread_cond_signal((&(ch)->cond_no_ref)); \
    }                                  \
    UNLOCK_RETURN(&((ch)->chan_lck), status);\
} while(0)

enum poll_event {
    POLL_EVENT_NON,
    POLL_EVENT_READABLE,
    POLL_EVENT_WRITABLE,
    POLL_EVENT_CLOSED,
};

typedef struct poll_req {
    sem_t * lck;         // this is to ensure that select caller can only be notified once
    sem_t * event_happen; // caller of select should wait on this
    size_t index; // what is the index of me in select list? caller of select should tell me that
    size_t* notifier; // I must tell select caller who notified him
    enum poll_event* p_event_type; // What is the event? Read， Write or an error?
} poll_req_t;

// poll request will include a pointer of semaphore

// why using semaphore here?
// conditional variables may miss out

// why do not use mutex here?
// for sem_trywait, the mutex can only be locked once.
static void init_poll_req(poll_req_t      *req,
                          sem_t           *lck,
                          sem_t           *event_happen,
                          size_t           index,
                          size_t          *notifier,
                          enum poll_event* p_event) {
    req->lck = lck;
    req->event_happen = event_happen;
    req->index = index;
    req->notifier = notifier;
    req->p_event_type = p_event;
}

// notify poller...
static bool notify_poller(poll_req_t* req, enum poll_event event) {
    DEBUG_EXPR(THREAD_PRINT("notify poller");)
    if (sem_trywait(req->lck)) {
        DEBUG_EXPR(THREAD_PRINT("poller has been notified or being notifying..");)
        return false;
    }
//    assert (*(req->p_event_type) == POLL_EVENT_NON);

    // it is not channel's responsibility to put data
    DEBUG_EXPR(THREAD_PRINT("I am notifying..");)
    *(req->notifier) = req->index;
    *(req->p_event_type) = event;
    sem_post(req->event_happen);
    return true;
}

// if there is a poller, then notify poller
// otherwise notify simple writers..
static void chan_notify_writer(chan_t* ch) {
    DEBUG_EXPR(THREAD_PRINT("chan_notify_writer");)
    list_node_t* begin ;
    while ((begin = list_begin(ch->wr_pollers)) != NULL) {
        poll_req_t* req = begin->data;
        list_remove(ch->wr_pollers, begin);
        if (req && notify_poller(req, POLL_EVENT_WRITABLE)) return;
    }
    pthread_cond_signal(&ch->cond_full);
}


// if there is a poller, then notify poller
// otherwise notify simple readers..
// two similar functions
static void chan_notify_reader(chan_t* ch) {
    DEBUG_EXPR(THREAD_PRINT("chan_notify_reader");)
    list_node_t* begin;
    while ((begin = list_begin(ch->rd_pollers))) {
        // some entry may notified, continue to notify next
        poll_req_t* req = begin->data;
        list_remove(ch->rd_pollers, begin);
        if (req && notify_poller(req, POLL_EVENT_READABLE)) return;
    }
    pthread_cond_signal(&ch->cond_empty);
}

// poll on channel
static enum chan_status channel_poll(
        select_t* poller,
        poll_req_t* req) {
    DEBUG_EXPR(THREAD_PRINT("channel poll to channel %p, is send is %d", poller->channel, poller->is_send);)
    chan_t* ch = poller->channel;
    if (poller->is_send) {
        ENTER_CHANNEL(ch);
        // try do a write call
        if (buffer_current_size(ch->buffer) <
                buffer_capacity(ch->buffer)) {
            // success
            buffer_add(poller->data,ch->buffer);
            // notify reader!
            chan_notify_reader(ch);
            // leave channel, end poll with success
            LEAVE_CHANNEL(ch, SUCCESS);
        } else {
            // add self to wait list
            // then waiting
            list_insert(ch->wr_pollers, req);
            UNLOCK_RETURN(&(ch->chan_lck), WOULDBLOCK);
        }
    } else {
        // similar logic
        ENTER_CHANNEL(ch);
        if (buffer_current_size(ch->buffer)) {
            poller->data = buffer_remove(ch->buffer);
            chan_notify_writer(ch);
            LEAVE_CHANNEL(ch, SUCCESS);
        } else {
            list_insert(ch->rd_pollers, req);
            UNLOCK_RETURN(&(ch->chan_lck), WOULDBLOCK);
        }
    }
}

// after select, all reqs that left
// in other channels should be removed
// otherwise it will confuse channel
static enum chan_status channel_cancel_poll(
        select_t* poller,
        poll_req_t* req) {
    DEBUG_EXPR(THREAD_PRINT("channel_cancel_poll");)
    chan_t* ch = poller->channel;
    list_t* list;
    list_node_t* node;
    pthread_mutex_lock(&ch->chan_lck);
    if (poller->is_send) {
        list = ch->wr_pollers;
        node = list_find(list,req);
        if (node) {
            list_remove(list, node);
        }
        LEAVE_CHANNEL(ch, SUCCESS);
    } else {
        list = ch->rd_pollers;
        node = list_find(list,req);
        if (node) {
            list_remove(list, node);
        }
        LEAVE_CHANNEL(ch, SUCCESS);
    }
}

// Creates a new channel with the provided size and returns it to the caller
// A 0 size indicates an unbuffered channel, whereas a positive size indicates a buffered channel
chan_t* channel_create(size_t size) {
    /* IMPLEMENT THIS */
    chan_t* ch = calloc(1, sizeof(chan_t));
    ch->buffer = buffer_create(size);
    ch->rd_pollers = list_create();
    ch->wr_pollers = list_create();
    pthread_mutex_init(&ch->chan_lck,NULL);
    pthread_cond_init(&ch->cond_empty, NULL);
    pthread_cond_init(&ch->cond_full, NULL);
    pthread_cond_init(&ch->cond_no_ref, NULL);
    return ch;
}

// Writes data to the given channel
// This can be both a blocking call i.e., the function only returns on a successful completion of send (blocking = true), and
// a non-blocking call i.e., the function simply returns if the channel is full (blocking = false)
// In case of the blocking call when the channel is full, the function waits till the channel has space to write the new data
// Returns SUCCESS for successfully writing data to the channel,
// WOULDBLOCK if the channel is full and the data was not added to the buffer (non-blocking calls only),
// CLOSED_ERROR if the channel is closed, and
// OTHER_ERROR on encountering any other generic error of any sort
enum chan_status channel_send(chan_t* ch, void* data, bool blocking) {
    /* IMPLEMENT THIS */
    DEBUG_EXPR(THREAD_PRINT("channel_send");)
    ENTER_CHANNEL(ch);

    if (blocking) {
        while (!ch->closed &&
                (buffer_current_size(ch->buffer) >=
                 buffer_capacity(ch->buffer))) {
            pthread_cond_wait(&ch->cond_full, &ch->chan_lck);
        }
        if (ch->closed) {
            LEAVE_CHANNEL(ch, CLOSED_ERROR);
        }
    } else {
        if (ch->closed) {
            LEAVE_CHANNEL(ch, CLOSED_ERROR);
        }
        if ((buffer_current_size(ch->buffer) >=
             buffer_capacity(ch->buffer))) {
            LEAVE_CHANNEL(ch, WOULDBLOCK);
        }
    }
    buffer_add(data, ch->buffer);
    chan_notify_reader(ch);
    LEAVE_CHANNEL(ch, SUCCESS);
}

// Reads data from the given channel and stores it in the function’s input parameter, data (Note that it is a double pointer).
// This can be both a blocking call i.e., the function only returns on a successful completion of receive (blocking = true), and
// a non-blocking call i.e., the function simply returns if the channel is empty (blocking = false)
// In case of the blocking call when the channel is empty, the function waits till the channel has some data to read
// Returns SUCCESS for successful retrieval of data,
// WOULDBLOCK if the channel is empty and nothing was stored in data (non-blocking calls only),
// CLOSED_ERROR if the channel is closed, and
// OTHER_ERROR on encountering any other generic error of any sort
enum chan_status channel_receive(chan_t* ch, void** data, bool blocking) {
    DEBUG_EXPR(THREAD_PRINT("channel_receive");)
    ENTER_CHANNEL(ch);

    if (blocking) {
        while (!ch->closed && !buffer_current_size(ch->buffer)) {
            pthread_cond_wait(&ch->cond_empty, &ch->chan_lck);
        }
        if (ch->closed) {
            LEAVE_CHANNEL(ch, CLOSED_ERROR);
        }
    } else {
        if (ch->closed) {
            LEAVE_CHANNEL(ch, CLOSED_ERROR);
        }
        if (!buffer_current_size(ch->buffer)) {
            LEAVE_CHANNEL(ch, WOULDBLOCK);
        }
    }
    *data = buffer_remove(ch->buffer);
    chan_notify_writer(ch);
    LEAVE_CHANNEL(ch, SUCCESS);
}

void notify_poller_closed(poll_req_t* req) {
    notify_poller(req, CLOSED_ERROR);
}

// Closes the channel and informs all the blocking send/receive/select calls to return with CLOSED_ERROR
// Once the channel is closed, send/receive/select operations will cease to function and just return CLOSED_ERROR
// Returns SUCCESS if close is successful,
// CLOSED_ERROR if the channel is already closed, and
// OTHER_ERROR in any other error case
enum chan_status channel_close(chan_t* ch) {
    DEBUG_EXPR(THREAD_PRINT("channel_close");)
    CHANNEL_LOCK(ch);
    if (ch->closed) UNLOCK_RETURN(&ch->chan_lck, CLOSED_ERROR);
    ch->closed = true;

    // notify all ordinary readers and writers...
    pthread_cond_broadcast(&ch->cond_full);
    pthread_cond_broadcast(&ch->cond_empty);

    // notify all pollers
    list_foreach(ch->rd_pollers, (void (*)(void *)) notify_poller_closed);
    list_foreach(ch->wr_pollers, (void (*)(void *)) notify_poller_closed);

    UNLOCK_RETURN(&ch->chan_lck, SUCCESS);
}

// Frees all the memory allocated to the channel
// The caller is responsible for calling channel_close and waiting for all threads to finish their tasks before calling channel_destroy
// Returns SUCCESS if destroy is successful,
// DESTROY_ERROR if channel_destroy is called on an open channel, and
// OTHER_ERROR in any other error case
enum chan_status channel_destroy(chan_t* ch) {
    CHANNEL_LOCK(ch);
    if (!ch->closed) UNLOCK_RETURN(&ch->chan_lck, DESTROY_ERROR);
    while (ch->ref_cnt > 0) {
        pthread_cond_wait(&ch->cond_no_ref, &ch->chan_lck);
    }
    pthread_mutex_unlock(&ch->chan_lck);

    buffer_free(ch->buffer);
    list_destroy(ch->rd_pollers);
    list_destroy(ch->wr_pollers);
    pthread_mutex_destroy(&ch->chan_lck);
    pthread_cond_destroy(&ch->cond_full);
    pthread_cond_destroy(&ch->cond_empty);
    pthread_cond_destroy(&ch->cond_no_ref);
    free(ch);
    return SUCCESS;
}


// Takes an array of channels, channel_list, of type select_t and the array length, channel_count, as inputs

// This API iterates over the provided list and finds the set of possible channels 
// which can be used to invoke the required operation (send or receive) specified in select_t

// If multiple options are available, it selects the first option and performs its corresponding action
// If no channel is available, the call is blocked and waits till it finds a channel which supports its required operation

// Once an operation has been successfully performed, select should set selected_index to the index of the channel 
// that performed the operation and then return SUCCESS

// In the event that a channel is closed or encounters any error, the error should be propagated and returned through select
// Additionally, selected_index is set to the index of the channel that generated the error
enum chan_status channel_select(size_t channel_count, select_t* channel_list, size_t* selected_index) {
    DEBUG_EXPR(THREAD_PRINT("channel_select");)
    size_t i, end_index;
    sem_t lck;
    sem_t sem_event;
    enum chan_status ret;
    select_t* selected;
    enum poll_event event_type;

    poll_req_t* reqs;

    sem_init(&lck, 0, 1);
    sem_init(&sem_event, 0, 0);
    reqs = calloc(channel_count, sizeof(poll_req_t));
    event_type = POLL_EVENT_NON;
    for (i = 0; i < channel_count; ++i) {
        init_poll_req(&reqs[i], &lck,
                      &sem_event,
                      i,
                      selected_index,
                      &event_type);
    }


    for (i = 0; i < channel_count; ++i) {
        ret = channel_poll(&channel_list[i], &reqs[i]);
        if (ret != WOULDBLOCK) {
            end_index = i;
            *selected_index = i;
            goto end_select;
        }
    }

    sem_wait(&sem_event);
    end_index = channel_count;
    selected = &channel_list[*selected_index];

    if (event_type >= POLL_EVENT_CLOSED) {
        ret = CLOSED_ERROR;
    } else if (selected->is_send && event_type == POLL_EVENT_WRITABLE) {
        ret = channel_send(selected->channel, selected->data, false);
    } else if (!selected->is_send && event_type == POLL_EVENT_READABLE){
        ret = channel_receive(selected->channel, &(selected->data), false);
    } else {
        ret = OTHER_ERROR;
    }

    end_select:
    for (i = 0; i < end_index; ++i) {
        channel_cancel_poll(&channel_list[i], &reqs[i]);
    }
    sem_destroy(&lck);
    sem_destroy(&sem_event);
    free(reqs);
    return ret;
}
