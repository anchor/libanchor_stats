#include "marquise.h"
#include "structs.h"
#include "frame.h"
#include "macros.h"
#include "../config.h"
#include "defer.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <zmq.h>
#include <stdio.h>
#include <glib.h>
#include <sys/mman.h>
#include "lz4/lz4.h"
#include "lz4/lz4hc.h"
#include <string.h>
#include <math.h>

#define LIBMARQUISE_PROFILING

static void debug_log( char *format, ...) {
        if( !getenv( "LIBMARQUISE_DEBUG" ) )
                return;

        va_list args;
        va_start( args, format );
        vfprintf( stderr, format, args );
        va_end( args );
}

#ifdef LIBMARQUISE_PROFILING
#define INC_COUNTER(_counter) ((_counter)++)
#else
#define INC_COUNTER(_counter)
#endif

static frame *accumulate_databursts( gpointer zmq_message, gint *queue_length )
{
        static frame *accumulator;
        static size_t offset = 0;

        // Reset and return result
        if( !zmq_message && !queue_length ) {
                debug_log( "Resetting accumulator\n" );

                frame *ret = accumulator;
                accumulator = NULL;
                offset = 0;
                return ret;
        }

        if( *queue_length <= 0 ) {
                return NULL;
        }

        if( !accumulator ) {
                debug_log( "Initializing accumulator for queue length %u\n"
                         , *queue_length );
                accumulator = calloc( *queue_length, sizeof( frame ) );
                if( !accumulator ) {
                        *queue_length -= 1;
                        return NULL;
                }
        }

        size_t msg_size = zmq_msg_size( zmq_message );
        accumulator[offset].length = msg_size;
        accumulator[offset].data = malloc( msg_size );
        if( !accumulator[offset].data ) {
                *queue_length -= 1;
                return NULL;
        }
        memcpy( accumulator[offset].data, zmq_msg_data( zmq_message ), msg_size );
        zmq_msg_close( zmq_message );
        free( zmq_message );
        offset++;

        return NULL;
}

// Bit twiddling is to support big endian architectures only. Really we are
// just writing two little endian uint32_t's which represent the original
// length, and the compressed length. The compressed data follows.
void add_header( uint8_t *dest, uint32_t original_len, uint32_t data_len ) {
        dest[3]= (original_len >> 24) & 0xff;
        dest[2]= (original_len >> 16) & 0xff;
        dest[1]= (original_len >> 8)  & 0xff;
        dest[0]=  original_len        & 0xff;

        dest[7]= (data_len >> 24) & 0xff;
        dest[6]= (data_len >> 16) & 0xff;
        dest[5]= (data_len >> 8)  & 0xff;
        dest[4]=  data_len        & 0xff;
}

// Returns -1 on failure, 0 on success
static int compress_burst( data_burst *burst ) {
        uint8_t *uncompressed = burst->data;
        // + 8 for the serialized data.
        burst->data = malloc( LZ4_compressBound( burst->length ) + 8 );
        if( !burst->data ) return -1;
        int written = LZ4_compressHC( (char *)uncompressed
                                    , (char *)burst->data + 8
                                    , (int)burst->length );
        if( !written ) return -1;
        add_header( burst->data, burst->length, written );
        burst->length = (size_t)written + 8;
        free( uncompressed );
        return 0;
}

// Collate a list of messages into a data burst, compress it, then send it over
// the supplied socket. This will free the given list.
static void send_message_list( GSList *message_list, void *destination_sock ) {
        if( !message_list ) return;

        debug_log("Sending queue\n");

        // This iterates over the entire list, passing each
        // element to accumulate_databursts
        guint list_length = g_slist_length( message_list );
        g_slist_foreach( g_slist_reverse( message_list )
                        , (GFunc)accumulate_databursts
                        , &list_length );
        g_slist_free( message_list );
        message_list = NULL;

        frame *frames = accumulate_databursts( NULL, NULL );
        data_burst *burst = malloc( sizeof( data_burst ) );
        size_t max_burst_length =
                get_databurst_size( frames, list_length );
        burst->data = malloc( max_burst_length );
        burst->length = aggregate_frames( frames
                                        , list_length
                                        , burst->data );
        debug_log( "Accumulated bursts serialized to %d bytes\n"
                 , burst->length);
        free( frames );
        fail_if( compress_burst( burst )
               , return;
               , "lz4 compression failed" );

        debug_log( "Accumulated bursts compressed to %d bytes\n"
                 , burst->length);

        int ret;
        do {
                ret = zmq_send( destination_sock
                              , burst->data
                              , burst->length
                              , 0 );
        } while( ret == -1 && errno == EINTR );

        free_databurst(burst);

        debug_log( "zmq_send() to poller returned %d\n", ret );

        fail_if( ret == -1
               ,  return;
               , "sending message: '%s'", strerror( errno ) );

        zmq_msg_t ack;
        zmq_msg_init( &ack );
        do {
                ret = zmq_msg_recv( &ack, destination_sock, 0 );
        } while( ret == -1  && errno == EINTR );

        fail_if( ret == -1
               ,  return;
               , "recieving internal ack: '%s'", strerror( errno ) );

        zmq_msg_close( &ack );
}

// This is the entrypoint for the collater thread.
//
// Here, we poll the client socket, passing collated and compressed data bursts
// through to the poller thread whenever a high water mark is reached or a
// timeout has triggered.

#define COLLATOR_MAX_MESSAGES 4096
#define COLLATOR_MAX_RX 131072 // 128 KB
static void *collator( void *args_p ) {
        collator_args *args = args_p;
        GTimer *timer = g_timer_new();
        GSList *message_list = NULL;
        gulong ms; // Useless, microseconds for gtimer_elapsed

        unsigned int water_mark            = 0;
        unsigned int rxed                  = 0;

        int poll_ms = floor( 1000 * args->poll_period );

        // Poll for frames from the client (a call to marquise_send_int for
        // example)
        zmq_pollitem_t items [] = {
                { args->client_sock
                , 0
                , ZMQ_POLLIN
                , 0 },
                { args->ipc_event_sock
                , 0
                , ZMQ_POLLIN
                , 0 },

        };

        while( 1 ) {
                // Calculate the poll period till the next timer expiry.
                int ms_elapsed = g_timer_elapsed( timer, &ms ) * 1000;
                int time_left = poll_ms - ms_elapsed;

                if( zmq_poll( items, 2,  time_left < 0 ? 0 : time_left ) == -1 ) {
                                syslog( LOG_ERR
                                , "libmarquise error: zmq_poll "
                                        "got unknown error code: %d"
                                , errno );
                                break;
                }

                if ( items [1].revents & ZMQ_POLLIN ) {
			// ipc message received
			//
			zmq_msg_t ipc_msg;
			zmq_msg_init(&ipc_msg);
			int rx = zmq_msg_recv( &ipc_msg, args->ipc_event_sock, 0 );

                        fail_if( rx == -1
                               , continue;
                               , "queue_loop: zmq_msg_recv: '%s'"
                               , strerror( errno ) );

                        // The client call to marquise_consumer_shutdown() will
                        // send this IPC message that instructs us to flush
                        // outstanding messages and shutdown. We later pass
                        // this on to the poller thread (after the break).
                        if(  rx == 3
                          && !strncmp( zmq_msg_data( &ipc_msg ), "DIE", 3 ) ) {
                                zmq_msg_close( &ipc_msg );
                                break;
                        }
			debug_log("libmarquise: poller received unknown IPC"
	                          "control message");
                        zmq_msg_close( &ipc_msg );
                        continue;
		}

                if ( items [0].revents & ZMQ_POLLIN ) {
                        zmq_msg_t *msg = malloc( sizeof( zmq_msg_t ) );
                        if( !msg ) continue;
                        zmq_msg_init( msg );
                        int rx = zmq_msg_recv( msg, args->client_sock, 0 );

                        fail_if( rx == -1
                               , free( msg ); continue;
                               , "queue_loop: zmq_msg_recv: '%s'"
                               , strerror( errno ) );

                        water_mark++;
                        rxed += rx;

                        // Prepend here so as to not traverse the entire list.
                        // We will reverse the list when we send it to preserve
                        // ordering.
                        message_list = g_slist_prepend( message_list, msg );
                }

                // Whenever the timer elapses or the high water mark is
                // reached, we collate our message list and then  send it to
                // the poller thread.
                if(  g_timer_elapsed( timer, &ms ) > args->poll_period
                  || water_mark >= COLLATOR_MAX_MESSAGES
                  || rxed >= COLLATOR_MAX_RX ) {
                        send_message_list( message_list, args->poller_sock  );

                        message_list = NULL;
                        g_timer_reset(timer);
                        water_mark = 0;
                        rxed = 0;
                }
        }

        // Flush the queue one last time before exit
        send_message_list( message_list, args->poller_sock );

        // All messages are sent, we now tell the poller thread to shutdown
        // cleanly and wait for it to send all messages upstream to the broker.
        int ret;
        do {
                ret = zmq_send( args->poller_sock, "DIE", 3, 0 );
        } while( ret == -1  && errno == EINTR );

        fail_if( ret == -1
               ,
               , "zmq_send: %s", strerror( errno ) );

        // Wait for the ack from the poller thread
        zmq_msg_t ack;
        zmq_msg_init( &ack );

        do {
                ret = zmq_recvmsg( args->poller_sock, &ack, 0 );
        } while (ret  == -1 && errno == EINTR );

        fail_if( ret == -1
               , return NULL;
               , "zmq_recvmsg: %s", strerror( errno ) );
        zmq_msg_close( &ack );


        // All done, we can send an ack to let marquise_consumer_shutdown
        // return
        if ( zmq_send( args->ipc_event_sock, "", 0, 0 ) < 0 )
		perror("zmq_send acking in marquise_consumer_shutdown\n");
        zmq_close( args->client_sock );
        zmq_close( args->poller_sock );
        zmq_close( args->ipc_event_sock );

        free( args );
        g_timer_destroy( timer );

        return NULL;
}

static inline uint64_t timestamp_now() {
        struct timespec ts;
        // This is used for timeouts, so we simply return 0 to timeout right
        // now.
        if( clock_gettime(CLOCK_REALTIME, &ts) )
                return 0;
        return ts.tv_sec * 1000000000 + ts.tv_nsec;
}


// Defer a zmq message to disk
static inline void defer_msg( zmq_msg_t *msg, deferral_file *df ) {
        data_burst burst;
        burst.data = zmq_msg_data( msg );
        burst.length = zmq_msg_size( msg );
        marquise_defer_to_file( df, &burst );
}

// Retrieve a zmq message from disk
static inline zmq_msg_t *retrieve_msg( deferral_file *df ) {
        data_burst *burst = marquise_retrieve_from_file( df );
        if( burst == NULL )
                return NULL;

        zmq_msg_t *msg = malloc( sizeof( zmq_msg_t ) );
        zmq_msg_init_size( msg, burst->length );
        memcpy( zmq_msg_data( msg ), burst->data, burst->length );
        free_databurst( burst );
        return msg;
}

// This can't be more than 65535 without changing the msg_id data type of the
// message_in_flight struct.
#define POLLER_HIGH_WATER_MARK 128

// Microseconds till a message expires
#define POLLER_EXPIRY 60000000000 // 60 seconds

// How often to check disk for a deferred message
#define POLLER_DEFER_PERIOD 1000000000 // 1 second

static void *poller( void *args_p ) {
        uint64_t defer_expiry = timestamp_now() + POLLER_DEFER_PERIOD;
        uint16_t msg_id = 0;
        poller_args *args = args_p;
        message_in_flight *in_flight =
                calloc( sizeof( message_in_flight ), POLLER_HIGH_WATER_MARK );
        message_in_flight *water_mark = in_flight - 1;
        const message_in_flight const *high_water_mark =
                &in_flight[POLLER_HIGH_WATER_MARK - 1];
        int shutting_down = 0;
        int read_success = 0;
        zmq_msg_t *deferred_msg = NULL;

#ifdef LIBMARQUISE_PROFILING
	uint64_t messages_in = 0;
	uint64_t messages_in_special = 0;
	uint64_t acks_sent = 0;
	uint64_t messages_sent_upstream = 0;
	uint64_t acks_received_from_upstream = 0;
	uint64_t messages_timed_out = 0;
	uint64_t messages_deferred_to_disk = 0;
	uint64_t messages_deferred_to_memory = 0;
	uint64_t messages_read_from_disk = 0;
	uint64_t poll_loops = 0;
#endif

        zmq_pollitem_t items[] = {
                // Poll from bursts coming from the collator thread
                { args->collator_sock
                , 0
                , ZMQ_POLLIN
                , 0 },
                // And for acks coming from the broker
                { args->upstream_sock
                , 0
                , ZMQ_POLLIN
                , 0 }
        };

        while( 1 ) {
                int poll_period = POLLER_DEFER_PERIOD / 1000;
                poll_period     = shutting_down? 100 : poll_period;
                poll_period     = read_success? 0 : poll_period;
                read_success    = 0;

                INC_COUNTER(poll_loops);
                if( zmq_poll( items, 2,  poll_period ) == -1 ) {
                                syslog( LOG_ERR
                                , "libmarquise error: zmq_poll "
                                        "got unknown error code: %d"
                                , errno );
                                break;
                }

                // Check for timeouts
                uint64_t now = timestamp_now();
                message_in_flight *i;
                for( i = in_flight; i <= water_mark; i++ )
                {
                        if( now > i->expiry ) {
                                // Expired, remove it from the list and
                                // defer to disk. Another possible option here
                                // would be to re-transmit to ourselves for
                                // immediate retry.
                                defer_msg( &i->msg, args->deferral_file );
                                zmq_msg_close( &i->msg );
                                *i = *water_mark;
                                water_mark--;
                                INC_COUNTER(messages_timed_out);
                                INC_COUNTER(messages_deferred_to_disk);
                        }
                }

                // Check if we need to get a deferred message
                deferred_msg = NULL;
                if( (defer_expiry > now  || shutting_down || read_success)
                  && water_mark != high_water_mark ) {
                        deferred_msg = retrieve_msg( args->deferral_file );
                        if( deferred_msg == NULL ) {
                                // Terminating case for shutdown, the disk
                                // currently has no messsages outstanding. If
                                // we have an empty in flight list, then we are
                                // ready to shutdown.
                                if( shutting_down && water_mark < in_flight ) {
                                        break;
                                }
                        } else {
                                read_success = 1;
                                INC_COUNTER(messages_read_from_disk);
                        }
                }

                // Check for incoming bursts from the collator thread, if we
                // have one, attempt to send upstream.
                if ( items [0].revents & ZMQ_POLLIN || deferred_msg != NULL ) {
                        zmq_msg_t *msg;
                        if( deferred_msg == NULL ) {
                                msg = malloc( sizeof( zmq_msg_t ) );
                                zmq_msg_init( msg );
                                int rx = zmq_msg_recv( msg, args->collator_sock, 0 );
                                fail_if( rx == -1
                                       , zmq_msg_close( msg ); free( msg ); continue;
                                       , "zmq_msg_recv: %s"
                                       , strerror( errno ) );

                                if(  rx == 3
                                && !strncmp( zmq_msg_data( msg ), "DIE", 3 ) ) {
                                        INC_COUNTER(messages_in_special);
                                        zmq_msg_close( msg );
                                        shutting_down = 1;
                                        free( msg );
                                        continue;
                                }
                                INC_COUNTER(messages_in);

                                // Ack immediately
                                fail_if( zmq_send( args->collator_sock, "", 0, 0 ) == -1
                                       ,
                                       , "zmq_send (collator ack)");
                                INC_COUNTER(acks_sent);
                        } else {
                                msg = deferred_msg;
                        }

                        if( water_mark < high_water_mark ) {
                                debug_log( "Poller sending message, free slots: %d / %d\n"
                                         , high_water_mark - water_mark
                                         , high_water_mark - in_flight );

                                // Move our water mark to the next avaliable slot
                                water_mark++;

                                // Send upstream
                                zmq_msg_init( &water_mark->msg );
                                zmq_msg_copy( &water_mark->msg, msg );

                                // Set time sent as now
                                water_mark->expiry = timestamp_now() + POLLER_EXPIRY;

                                // Tack on our message id;
                                msg_id++;
                                water_mark->msg_id = msg_id;

                                #define TRANSMIT_CLEANUP {                     \
                                        defer_msg( msg, args->deferral_file ); \
                                        zmq_msg_close( msg );                  \
                                        zmq_msg_close( &water_mark->msg );     \
                                        free( msg );                           \
                                        water_mark--;                          \
                                }

                                int tx;
                                do {
                                        tx = zmq_send( args->upstream_sock
                                                , (void *)&msg_id
                                                , sizeof(msg_id)
                                                , ZMQ_SNDMORE );
                                } while( tx == -1 && errno == EINTR );

                                debug_log( "zmq_send() of msg_id to broker returned %d\n", tx );

                                fail_if( tx == -1
                                       , TRANSMIT_CLEANUP; continue;
                                       , "zmq_send: %s"
                                       , strerror( errno ) );
                                do {
                                        tx = zmq_sendmsg( args->upstream_sock
                                                    , msg
                                                    , 0 );
                                } while( tx == -1 && errno == EINTR );

                                debug_log( "zmq_send() of burst to broker returned %d\n", tx );

                                fail_if( tx == -1
                                       , TRANSMIT_CLEANUP; continue;
                                         INC_COUNTER(messages_deferred_to_disk);
                                       , "zmq_send: %s"
                                       , strerror( errno ) );

                                INC_COUNTER(messages_sent_upstream);
                        } else {
                                // Defer to disk as there are already too many
                                // messages in flight.
                                defer_msg( msg, args->deferral_file );
                                zmq_msg_close( msg );
                                INC_COUNTER(messages_deferred_to_disk);
                        }

                        free( msg );
                }


                // Check for acks coming from the broker, remove them
                // from our inflight list if they match.
                if( items [1].revents & ZMQ_POLLIN ) {
                        // The first part of the message is the
                        // message ID, the second is the ack. The ack
                        // is empty on success and contains an error
                        // message on failure.
                        zmq_msg_t msg_id, ack;
                        zmq_msg_init( &msg_id );
                        zmq_msg_init( &ack );

                        int rx = zmq_msg_recv( &msg_id, args->upstream_sock, 0 );

                        debug_log( "zmq_recv() of ack id from broker returned %d\n", rx );

                        if( rx != sizeof(uint16_t) ) {
                                zmq_msg_close( &msg_id );
                                zmq_msg_close( &ack );
                                continue;
                        }

                        rx = zmq_msg_recv( &ack, args->upstream_sock, 0 );
                        if( rx == -1 ) {
                                zmq_msg_close( &msg_id );
                                zmq_msg_close( &ack );
                                continue;
                        }

                        debug_log( "zmq_recv() of ack msg from broker returned %d\n", rx );

                        // Both failure and success require removal
                        // from our inflight list, so we treat both the
                        // same in terms of searching and removal.
                        // Which we shall do now:

                        message_in_flight *i;
                        uint16_t *ack_msg_id = zmq_msg_data( &msg_id );


                        // If we have a match, remove this element from
                        // the inflight list by replacing it with the
                        // last element then decrementing the
                        // water_mark pointer.
                        for( i = in_flight; i <= water_mark; i++ )
                        {
                                if( i->msg_id == *ack_msg_id ) {
                                        zmq_msg_close( &i->msg );
                                        *i = *water_mark;
                                        water_mark--;
                                        INC_COUNTER(acks_received_from_upstream);
                                }
                        }

                        // If we got an error, syslog will want to know about it.
                        // This means data loss.
                        fail_if( rx > 0
                               ,
                               , "recieved error from broker: '%.*s'"
                               , rx
                               , (char *)zmq_msg_data( &ack ) );

                        zmq_msg_close( &msg_id );
                        zmq_msg_close( &ack );
                }
        }

        // Send the ack to notify that we have shutdown.
        zmq_send( args->collator_sock, "", 0, 0 );
        zmq_close( args->collator_sock );
        zmq_close( args->self_sock );
        zmq_close( args->upstream_sock );

        // TODO: Assert that the deferral file is empty
        marquise_deferral_file_close( args->deferral_file );
        marquise_deferral_file_free( args->deferral_file );

        free( args );
        free( in_flight );

#ifdef LIBMARQUISE_PROFILING
        debug_log("libmarquise poll thread profiling:\n"
                "        messages_in = %lu\n"
                "        messages_in_special = %lu\n"
                "        acks_sent = %lu\n"
                "        messages_sent_upstream = %lu\n"
                "        acks_received_from_upstream = %lu\n"
                "        messages_timed_out = %lu\n"
                "        messages_deferred_to_disk = %lu\n"
                "        messages_deferred_to_memory = %lu\n"
                "        messages_read_from_disk = %lu\n"
                "        poll_loops = %lu\n",
	messages_in,
	messages_in_special,
	acks_sent,
	messages_sent_upstream,
	acks_received_from_upstream,
	messages_timed_out,
	messages_deferred_to_disk,
	messages_deferred_to_memory,
	messages_read_from_disk,
	poll_loops);
#endif

        return NULL;
}

#define ctx_fail_if( assertion, action, ... )             \
        fail_if( assertion                                \
               , { action };                              \
                 zmq_ctx_destroy( context );              \
                 return NULL;                             \
               , "marquise_consumer_new: " __VA_ARGS__ ); \

marquise_consumer marquise_consumer_new( char *broker, double poll_period ) {
	consumer_state *consumer;
       	consumer = malloc(sizeof(consumer_state));
	fail_if( !consumer
		, perror("malloc");
		, "malloc" );

        // One context per consumer, this is passed back to the caller as an
        // opaque pointer.
        void *context = zmq_ctx_new();
        fail_if( !context
               , return NULL;
               , "zmq_ctx_new failed, this is very confusing." );

        ctx_fail_if( poll_period <= 0, , "poll_period cannot be <= 0" );

        // Set up the client facing REP socket.
        void *collator_pull_socket = zmq_socket( context, ZMQ_PULL );
        ctx_fail_if( !collator_pull_socket
                   ,
                   , "zmq_socket: '%s'"
                   , strerror( errno ) );

        ctx_fail_if( zmq_bind( collator_pull_socket, "inproc://collator" )
                   , zmq_close( collator_pull_socket );
                   , "zmq_bind: '%s'"
                   , strerror( errno ) );

	// and a client facing out-of-band IPC socket for the rare
	// synchronous messages (only 'DIE' at this point)
        void *collator_ipc_req_socket = zmq_socket( context, ZMQ_REQ );
        void *collator_ipc_rep_socket = zmq_socket( context, ZMQ_REP );
        ctx_fail_if( !collator_ipc_req_socket
                   ,
                   , "zmq_socket: '%s'"
                   , strerror( errno ) );
        ctx_fail_if( !collator_ipc_rep_socket
                   ,
                   , "zmq_socket: '%s'"
                   , strerror( errno ) );

        ctx_fail_if( zmq_bind( collator_ipc_rep_socket, "inproc://collator_ipc" )
                   , zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_bind: '%s'"
                   , strerror( errno ) );
        ctx_fail_if( zmq_connect( collator_ipc_req_socket, "inproc://collator_ipc" )
                   , zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_connect: '%s'"
                   , strerror( errno ) );

        // And then the internal collater to poller sockets, note that the bind
        // must happen before the connect, this is a "feature" of inproc
        // sockets.
        void *poller_rep_socket = zmq_socket( context, ZMQ_REP );
        ctx_fail_if( !poller_rep_socket
                   , zmq_close( collator_pull_socket );
                   , "zmq_socket: '%s'"
                   , strerror( errno ) );

        ctx_fail_if( zmq_bind( poller_rep_socket, "inproc://poller" )
                   , zmq_close( poller_rep_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_bind: '%s'"
                   , strerror( errno ) );

        void *poller_req_socket = zmq_socket( context, ZMQ_REQ );
        ctx_fail_if( !poller_req_socket
                   , zmq_close( poller_rep_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_socket: '%s'"
                   , strerror( errno ) );

        ctx_fail_if( zmq_connect( poller_req_socket, "inproc://poller" )
                   , zmq_close( poller_req_socket );
                     zmq_close( poller_rep_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_connect: '%s'"
                   , strerror( errno ) );

        // The poller wants to be able to talk to itself.
        void *poller_req_self_socket = zmq_socket( context, ZMQ_REQ );
        ctx_fail_if( !poller_req_self_socket
                   , zmq_close( poller_req_socket );
                     zmq_close( poller_rep_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_socket: '%s'"
                   , strerror( errno ) );

        ctx_fail_if( zmq_connect( poller_req_self_socket, "inproc://poller" )
                   , zmq_close( poller_req_socket );
                     zmq_close( poller_rep_socket );
                     zmq_close( poller_req_self_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                   , "zmq_connect: '%s'"
                   , strerror( errno ) );

        // Finally, the upstream socket (connecting to the broker)
        void *upstream_dealer_socket = zmq_socket( context, ZMQ_DEALER );
        ctx_fail_if( !upstream_dealer_socket
                   , zmq_close( poller_rep_socket );
                     zmq_close( poller_req_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                     zmq_close( poller_req_self_socket );
                  , "zmq_socket: '%s'"
                  , strerror( errno ) );

        ctx_fail_if( zmq_connect( upstream_dealer_socket, broker )
                   , zmq_close( poller_rep_socket );
                     zmq_close( poller_req_socket );
                     zmq_close( collator_pull_socket );
                     zmq_close( collator_ipc_req_socket );
                     zmq_close( collator_ipc_rep_socket );
                     zmq_close( poller_req_self_socket );
                     zmq_close( upstream_dealer_socket );
                   , "zmq_connect: '%s'"
                   , strerror( errno ) );

        // All of our setup is done, most things that could have failed would
        // have by now.
        //
        // We now initialize two threads, a collator and a poller.
        //
        // The collator listens to frames sent by the user and periodically
        // flushes them to the poller when the high water mark is reached or
        // when a timer has elapsed. This way we get regular bursts into the
        // poller.
        //
        // The poller communicates with the broker, sending bursts recieved
        // from the collator upstream.

        // We will want to wrap up soon as this state is becoming messy:
        #define CLEANUP { zmq_close( poller_req_socket );      \
                          zmq_close( poller_rep_socket );      \
                          zmq_close( poller_req_self_socket ); \
                          zmq_close( collator_pull_socket );    \
                          zmq_close( collator_ipc_req_socket );    \
                          zmq_close( collator_ipc_rep_socket );    \
                          zmq_close( upstream_dealer_socket ); }


        // Set up the arguments for the collator thread and start that.
        collator_args *ca = malloc( sizeof( collator_args ) );
        ctx_fail_if( !ca, CLEANUP, "malloc" );

        ca->client_sock = collator_pull_socket;
        ca->poller_sock = poller_req_socket;
	ca->ipc_event_sock = collator_ipc_rep_socket;
        ca->poll_period = poll_period;

        pthread_t collator_pthread;
        int err = pthread_create( &collator_pthread
                                , NULL
                                , collator
                                , ca );
        ctx_fail_if( err
                   , CLEANUP
                   , "pthread_create returned: '%d'", err );

        err = pthread_detach( collator_pthread );
        ctx_fail_if( err
                   , CLEANUP
                   , "pthread_detach returned: '%d'", err );

        // The collator is running, now we start the poller thread.
        poller_args *pa = malloc( sizeof( poller_args ) );
        ctx_fail_if( !pa, CLEANUP, "malloc" );

        pa->upstream_sock = upstream_dealer_socket;
        pa->collator_sock = poller_rep_socket;
        pa->self_sock     = poller_req_self_socket;

        pa->deferral_file = marquise_deferral_file_new();
        ctx_fail_if( ! pa->deferral_file
                   , CLEANUP
                   , "mkstemp: '%s'"
                   , strerror( errno ) );

        pthread_t poller_pthread;
        err = pthread_create( &poller_pthread
                                , NULL
                                , poller
                                , pa );
        ctx_fail_if( err
                   , CLEANUP
                   , "pthread_create returned: '%d'", err );

        err = pthread_detach( poller_pthread );
        ctx_fail_if( err
                   , CLEANUP
                   , "pthread_detach returned: '%d'", err );

        // Ready to go as far as we're concerned
	// save the context and ipc sockets and return the consumer
	// state
	consumer->context = context;
	consumer->collator_ipc_event_req_sock = collator_ipc_req_socket;
        return (marquise_consumer) consumer;
}

void marquise_consumer_shutdown( marquise_consumer consumer ) {
	consumer_state *cs = (consumer_state *) consumer;

        int tx;
        do {
                tx = zmq_send( cs->collator_ipc_event_req_sock, "DIE", 3, 0 );
        } while( tx == -1 && errno == EINTR );
        fail_if( tx == -1, return;, "zmq_send (DIE)")

        // The consumer thread will signal when it is done cleaning up.
        zmq_msg_t ack;
        zmq_msg_init( &ack );

        int rx;
        do {
                rx = zmq_recvmsg( cs->collator_ipc_event_req_sock, &ack, 0 );
        } while( rx == -1 && errno == EINTR );
        fail_if( rx == -1
               , return;
               , "zmq_recvmsg: %s", strerror( errno ) );

        zmq_msg_close( &ack );
        zmq_close( cs->collator_ipc_event_req_sock );
        zmq_ctx_destroy( cs->context );
	free(cs);
}

#define conn_fail_if( assertion, ... ) \
        fail_if( assertion                             \
               , zmq_close( connection ); return NULL; \
               , __VA_ARGS__ );                        \

marquise_connection marquise_connect( marquise_consumer consumer ) {
	consumer_state *cs = (consumer_state *) consumer;

        void *connection = zmq_socket( cs->context, ZMQ_PUSH );
        conn_fail_if( !connection
                    , "marquise_connect: zmq_socket: '%s'"
                    , strerror( errno ) );
        conn_fail_if( zmq_connect( connection, "inproc://collator" )
                    , "marquise_connect: zmq_connect: '%s'"
                    , strerror( errno ) );
        return connection;
}

void marquise_close( marquise_connection connection ) {
        zmq_close( connection );
}

int marquise_send_frame( marquise_connection connection
                 , DataFrame *frame) {
        size_t length = data_frame__get_packed_size( frame );
        uint8_t *marshalled_frame = malloc( length );
        if( !marshalled_frame ) return -1;

        data_frame__pack( frame, marshalled_frame );
        free_frame( frame );

        int ret;
        retry_send:
        ret = zmq_send( connection, marshalled_frame, length, 0);
        if( ret == -1  && errno == EINTR )
                goto retry_send;

        free( marshalled_frame );
        return ret;
}

int marquise_send_text( marquise_connection connection
           , char **source_fields
           , char **source_values
           , size_t source_count
           , char *data
           , size_t length
           , uint64_t timestamp) {
        DataFrame *frame = build_frame( source_fields
                                      , source_values
                                      , source_count
                                      , timestamp
                                      , DATA_FRAME__TYPE__TEXT );
        if( !frame ) return -1;
        frame->value_textual = data;
        return marquise_send_frame( connection, frame );
}

int marquise_send_int( marquise_connection connection
           , char **source_fields
           , char **source_values
           , size_t source_count
           , int64_t data
           , uint64_t timestamp) {
        DataFrame *frame = build_frame( source_fields
                                      , source_values
                                      , source_count
                                      , timestamp
                                      , DATA_FRAME__TYPE__NUMBER);
        if( !frame ) return -1;
        frame->value_numeric = data;
        frame->has_value_numeric = 1;
        return marquise_send_frame( connection, frame );
}

int marquise_send_real( marquise_connection connection
           , char **source_fields
           , char **source_values
           , size_t source_count
           , double data
           , uint64_t timestamp) {
        DataFrame *frame = build_frame( source_fields
                                      , source_values
                                      , source_count
                                      , timestamp
                                      , DATA_FRAME__TYPE__REAL);
        if( !frame ) return -1;
        frame->value_measurement = data;
        frame->has_value_measurement = 1;
        return marquise_send_frame( connection, frame );
}

int marquise_send_counter( marquise_connection connection
           , char **source_fields
           , char **source_values
           , size_t source_count
           , uint64_t timestamp) {
        DataFrame *frame = build_frame( source_fields
                                      , source_values
                                      , source_count
                                      , timestamp
                                      , DATA_FRAME__TYPE__EMPTY);
        if( !frame ) return -1;
        return marquise_send_frame( connection, frame );
}

int marquise_send_binary( marquise_connection connection
           , char **source_fields
           , char **source_values
           , size_t source_count
           , uint8_t *data
           , size_t length
           , uint64_t timestamp) {
        DataFrame *frame = build_frame( source_fields
                                      , source_values
                                      , source_count
                                      , timestamp
                                      , DATA_FRAME__TYPE__BINARY);
        if( !frame ) return -1;
        frame->value_blob.len = length;
        frame->value_blob.data = data;
        frame->has_value_blob = 1;
        return marquise_send_frame( connection, frame );
}
