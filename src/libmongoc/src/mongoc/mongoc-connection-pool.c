#include "mongoc-config.h"

#include "mongoc-connection-pool-private.h"
#include "mongoc-host-list-private.h"


mongoc_server_stream_t *
mongoc_checkout_connection (mongoc_connection_pool_t *connection_pool,
                            mongoc_cluster_t *cluster,
                            bson_error_t *error)
{
   mongoc_stream_t *stream;
   mongoc_topology_t *topology;
   uint32_t server_id;
   mongoc_server_stream_t *server_stream;
   uint32_t generation;

   bson_mutex_lock (&connection_pool->mutex);
   server_id = connection_pool->server_id;
   topology = connection_pool->topology;
   generation = connection_pool->generation;

again:
   if (connection_pool->available_connections) {
      server_stream = _mongoc_queue_pop_head (connection_pool->queue);
      BSON_ASSERT (server_stream);
      connection_pool->available_connections--;
   }
   else if (connection_pool->total_connections < topology->max_connection_pool_size) {
      connection_pool->total_connections++;
      bson_mutex_unlock (&connection_pool->mutex);
      stream = _mongoc_cluster_add_node (cluster, generation,
                                             server_id, error);

      if (!stream) {
         bson_mutex_lock (&connection_pool->mutex);
         connection_pool->total_connections--;
         connection_pool->available_connections--;
         bson_mutex_unlock (&connection_pool->mutex);
         return NULL;
      }
      server_stream = _mongoc_cluster_create_server_stream (topology,
                                                            server_id,
                                                            stream, error);
      server_stream->server_id = server_id;
      bson_mutex_lock (&connection_pool->mutex);
      server_stream->connection_id = ++connection_pool->max_id;

   }
   else {
      mongoc_cond_t new_cond;
      mongoc_cond_init (&new_cond);
      _mongoc_queue_push_head (connection_pool->cond_queue, &new_cond);
      mongoc_cond_wait (&new_cond, &connection_pool->mutex);
      goto again;
   }
   bson_mutex_unlock (&connection_pool->mutex);
   return server_stream;
}

void
mongoc_checkin_connection (mongoc_connection_pool_t *connection_pool,
                           mongoc_server_stream_t *server_stream)
{
   mongoc_cond_t *cond;
   bson_mutex_lock (&connection_pool->mutex);
   connection_pool->available_connections++;
   cond = _mongoc_queue_pop_tail (connection_pool->cond_queue);
   _mongoc_queue_push_head (connection_pool->queue, server_stream);
   if (cond)
      mongoc_cond_signal (cond);
   bson_mutex_unlock (&connection_pool->mutex);
}

mongoc_connection_pool_t *
mongoc_connection_pool_new (mongoc_topology_t *topology,
                            mongoc_server_description_t *sd)
{
   mongoc_connection_pool_t *new_pool =
      bson_malloc0 (sizeof (mongoc_connection_pool_t));
   new_pool->server_id = sd->id;
   new_pool->max_id = 0;
   new_pool->generation = 0;
   new_pool->total_connections = 0;
   new_pool->available_connections = 0;
   new_pool->topology = topology;
   bson_mutex_init (&new_pool->mutex);
   new_pool->queue = bson_malloc (sizeof (mongoc_queue_t));
   new_pool->cond_queue = bson_malloc (sizeof (mongoc_queue_t));
   _mongoc_queue_init (new_pool->queue);
   _mongoc_queue_init (new_pool->cond_queue);
   return new_pool;
}

void
mongoc_connection_pool_close (mongoc_connection_pool_t *pool)
{
   mongoc_queue_t *queue = pool->queue;
   mongoc_server_stream_t *curr;
   bson_mutex_lock (&pool->mutex);
   while ((curr = _mongoc_queue_pop_head (queue))) {
      mongoc_stream_close (curr->stream);
      mongoc_server_stream_cleanup (curr);
   }
   bson_mutex_unlock (&pool->mutex);
}
