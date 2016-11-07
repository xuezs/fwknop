/*
 * connection_tracker.c
 *
 *  Created on: Sep 29, 2016
 *      Author: Daniel Bailey
 */

//#ifdef FIREWALL_IPTABLES


#include "fwknopd_common.h"
#include "fwknopd_errors.h"
#include "utils.h"
#include "log_msg.h"
#include "extcmd.h"
#include "access.h"
#include "bstrlib.h"
#include "hash_table.h"
#include "sdp_ctrl_client.h"
#include <json-c/json.h>
#include <fcntl.h>
#include "connection_tracker.h"

const char *conn_id_key = "connection_id";
const char *sdp_id_key  = "sdp_id";

static hash_table_t *connection_hash_tbl = NULL;
static hash_table_t *latest_connection_hash_tbl = NULL;
static uint64_t last_conn_id = 0;
static connection_t msg_conn_list = NULL;
static int verbosity = 0;
static time_t next_ctrl_msg_due = 0;

static void print_connection_item(connection_t this_conn)
{
	char start_str[100] = {0};
	char end_str[100] = {0};

	memcpy(start_str, ctime( &(this_conn->start_time) ), 100);

	memcpy(end_str,
		   this_conn->end_time ? ctime( &(this_conn->end_time)) : "connection open\n",
		   100);

    log_msg(LOG_WARNING,
            "    Conn ID:  %"PRIu64"\n"
            "     SDP ID:  %"PRIu32"\n"
            "     src ip:  %s\n"
            "   src port:  %u\n"
            "     dst ip:  %s\n"
            "   dst port:  %u\n"
            " start time:  %s"
            "   end time:  %s"
			"       next:  %p\n\n",
            this_conn->connection_id,
            this_conn->sdp_id,
            this_conn->src_ip_str,
            this_conn->src_port,
            this_conn->dst_ip_str,
            this_conn->dst_port,
			start_str, //ctime( &(this_conn->start_time) ),
			end_str, //this_conn->end_time ? ctime( &(this_conn->end_time)) : "connection open\n",
            this_conn->next );

}


static void print_connection_list(connection_t conn)
{
    while(conn != NULL)
    {
        print_connection_item(conn);
        conn = conn->next;
    }

    log_msg(LOG_WARNING, "\n");
}


static void destroy_connection_item(connection_t item)
{
    free(item);
}


static void destroy_connection_list(connection_t list)
{
    connection_t this_conn = list;
    connection_t next = NULL;

    // if(verbosity >= LOG_DEBUG)
    // {
    //     while(this_conn != NULL)
    //     {
    //         fprintf(stderr, "Destroying connection item:\n");
    //         print_connection_item(this_conn, NULL);
    //         this_conn = this_conn->next;
    //     }
    //
    //     this_conn = list;
    // }

    while(this_conn != NULL)
    {
        next = this_conn->next;
        destroy_connection_item(this_conn);
        this_conn = next;
    }
}


static int validate_connection(acc_stanza_t *acc, connection_t conn, int *valid_r)
{
    char *spot = NULL;
    char port_str[10];

    *valid_r = 0;

    if( !(acc && conn) )
    {
    	log_msg(LOG_ERR, "validate_connection() ERROR: NULL arg passed");
    	return FWKNOPD_ERROR_CONNTRACK;
    }

    memset(port_str, 0x0, 10);
    snprintf(port_str, 9, "%d", conn->dst_port);

    // check if the dest port is in open_ports list
    if( (spot = strstr(acc->open_ports, port_str)) != NULL)
    	*valid_r = 1;
    else
    {
    	log_msg(LOG_WARNING, "validate_connection() found invalid connection:");
    	print_connection_item(conn);
    }

    return FWKNOPD_SUCCESS;
}


static int add_to_connection_list(connection_t *list, connection_t new_guy)
{
    connection_t this_conn = *list;

    if(!new_guy)
    {
        log_msg(LOG_ERR,
                "add_to_connection_list() Error: NULL argument passed");
        return FWKNOPD_ERROR_CONNTRACK;
    }

    if(*list == NULL)
    {
        *list = new_guy;
        return FWKNOPD_SUCCESS;
    }

    while(this_conn->next != NULL)
        this_conn = this_conn->next;

    this_conn->next = new_guy;

    return FWKNOPD_SUCCESS;
}


static int create_connection_item( uint64_t connection_id,
                                   uint32_t sdp_id,
                                   char *src_ip_str,
                                   unsigned int src_port,
                                   char *dst_ip_str,
                                   unsigned int dst_port,
                                   time_t start_time,
                                   time_t end_time,
                                   connection_t *this_conn_r
                                 )
{
    connection_t this_conn = calloc(1, sizeof *this_conn);

    if(this_conn == NULL)
    {
        log_msg(LOG_ERR, "create_connection_item() FATAL MEMORY ERROR. ABORTING.");
        *this_conn_r = NULL;
        return FWKNOPD_ERROR_MEMORY_ALLOCATION;
    }

    this_conn->connection_id = connection_id;
    this_conn->sdp_id     = sdp_id;
    strncpy(this_conn->src_ip_str, src_ip_str, MAX_IPV4_STR_LEN);
    this_conn->src_port   = src_port;
    strncpy(this_conn->dst_ip_str, dst_ip_str, MAX_IPV4_STR_LEN);
    this_conn->dst_port   = dst_port;
    this_conn->start_time = start_time;
    this_conn->end_time   = end_time;

    *this_conn_r = this_conn;

    return FWKNOPD_SUCCESS;
}


static int create_connection_item_from_line(const char *line, time_t now, connection_t *this_conn_r)
{
    int res = FWKNOPD_SUCCESS;
    connection_t this_conn = NULL;
    char *ndx = NULL;
    unsigned int id = 0;


    // first determine if 'mark' is nonzero
    if( (ndx = strstr(line, "mark=")) == NULL)
    {
        *this_conn_r = NULL;
        return FWKNOPD_SUCCESS;
    }

    id = 0;
    if( !sscanf((ndx+strlen("mark=")), "%u", &id) )
    {
        log_msg(LOG_ERR, "create_connection_item_from_line() ERROR: failed to extract "
                "'mark' value from conntrack line:\n     %s\n", line);
        *this_conn_r = NULL;
        return FWKNOPD_SUCCESS;
    }

    if( id == 0 )
    {
        *this_conn_r = NULL;
        return FWKNOPD_SUCCESS;
    }

    // get the connection details
    if( (ndx = strstr(line, "src=")) == NULL)
    {
        log_msg(LOG_ERR, "create_connection_item_from_line() Failed to find start of "
                "connection details in line: \n     %s\n", line);
        *this_conn_r = NULL;
        return FWKNOPD_SUCCESS;
    }

    if( (this_conn = calloc(1, sizeof *this_conn)) == NULL)
    {
        log_msg(LOG_ERR, "create_connection_item_from_line() FATAL MEMORY ERROR. ABORTING.");
        *this_conn_r = NULL;
        return FWKNOPD_ERROR_MEMORY_ALLOCATION;
    }


    if( (res = sscanf(ndx, "src=%15s dst=%15s sport=%u dport=%u",
               this_conn->src_ip_str,
               this_conn->dst_ip_str,
               &(this_conn->src_port),
               &(this_conn->dst_port) )) != 4 )
    {
        log_msg(LOG_ERR, "create_connection_item_from_line() Failed to find "
                "connection details in line: \n     %s\n", ndx);
        destroy_connection_item(this_conn);
        *this_conn_r = NULL;
        return FWKNOPD_SUCCESS;
    }

    this_conn->sdp_id = (uint32_t)id;
    this_conn->start_time = now;

    *this_conn_r = this_conn;
    return FWKNOPD_SUCCESS;
}


static int search_conntrack(fko_srv_options_t *opts,
                            char *criteria,
                            connection_t *conn_list_r,
                            int *conn_count_r)
{
    char   cmd_buf[CMD_BUFSIZE];
    char   cmd_out[STANDARD_CMD_OUT_BUFSIZE];
    int    conn_count = 0, res = FWKNOPD_SUCCESS;
    time_t now;
    int pid_status = 0;
    char *line = NULL;
    connection_t this_conn = NULL;
    connection_t conn_list = NULL;

    time(&now);

    memset(cmd_buf, 0x0, CMD_BUFSIZE);
    memset(cmd_out, 0x0, STANDARD_CMD_OUT_BUFSIZE);

    if(criteria != NULL)
        snprintf(cmd_buf, CMD_BUFSIZE-1, "conntrack -L %s", criteria);
    else
        snprintf(cmd_buf, CMD_BUFSIZE-1, "conntrack -L");

    res = run_extcmd(cmd_buf, cmd_out, STANDARD_CMD_OUT_BUFSIZE,
            WANT_STDERR, NO_TIMEOUT, &pid_status, opts);
    chop_newline(cmd_out);

    if(!EXTCMD_IS_SUCCESS(res))
    {
        log_msg(LOG_ERR,
                "search_conntrack() Error %i from cmd:'%s': %s",
                res, cmd_buf, cmd_out);
        return FWKNOPD_ERROR_CONNTRACK;
    }

    line = strtok(cmd_out, "\n");
    log_msg(LOG_DEBUG, "search_conntrack() first line from conntrack call: \n"
            "    %s\n", line);

    // don't want first line, so move to second line
    if(line != NULL)
       line = strtok(NULL, "\n");

    // walk through the rest of the lines
    while( line != NULL )
    {
        // extract connection info from line and create connection item
        if( (res = create_connection_item_from_line(line, now, &this_conn)) != FWKNOPD_SUCCESS)
        {
            destroy_connection_list(conn_list);
            return res;
        }

        if(this_conn == NULL)
        {
            // line did not represent a connection of interest, carry on
            line = strtok(NULL, "\n");
            continue;
        }

        if( (res = add_to_connection_list(&conn_list, this_conn)) != FWKNOPD_SUCCESS)
        {
        	destroy_connection_item(this_conn);
            destroy_connection_list(conn_list);
            return res;
        }

        conn_count++;

        line = strtok(NULL, "\n");
    }

    *conn_list_r = conn_list;
    *conn_count_r = conn_count;

    return res;
}


static int close_connections(fko_srv_options_t *opts, char *criteria)
{
    char   cmd_buf[CMD_BUFSIZE];
    char   cmd_out[STANDARD_CMD_OUT_BUFSIZE];
    int    conn_count = 0, res = FWKNOPD_SUCCESS;
    int pid_status = 0;
    connection_t conn_list = NULL;

    if(criteria == NULL)
    {
    	log_msg(LOG_WARNING, "close_connections() null criteria passed, nothing to close");
    	return res;
    }

    memset(cmd_buf, 0x0, CMD_BUFSIZE);
    memset(cmd_out, 0x0, STANDARD_CMD_OUT_BUFSIZE);

    snprintf(cmd_buf, CMD_BUFSIZE-1, "conntrack -D %s", criteria);

    res = run_extcmd(cmd_buf, cmd_out, STANDARD_CMD_OUT_BUFSIZE,
		             WANT_STDERR, NO_TIMEOUT, &pid_status, opts);
	chop_newline(cmd_out);

    if(!EXTCMD_IS_SUCCESS(res))
    {
		log_msg(LOG_ERR, "close_connections() Error %i from cmd:'%s': %s",
				res, cmd_buf, cmd_out);
		return FWKNOPD_ERROR_CONNTRACK;
    }

    if( (res = search_conntrack(opts, criteria, &conn_list, &conn_count)) != FWKNOPD_SUCCESS)
    {
    	log_msg(LOG_ERR, "close_connections() Error when trying to verify connections were closed");
    	return res;
    }

    if(conn_count != 0)
    {
    	log_msg(LOG_ERR, "close_connections() Failed to close the following connections:");
    	print_connection_list(conn_list);
    	return FWKNOPD_ERROR_CONNTRACK;
    }

    log_msg(LOG_WARNING, "Gateway closed connections meeting the following criteria:\n"
    		             "     %s \n", criteria);

    return res;
}


static int duplicate_connection_item(connection_t orig, connection_t *copy)
{
    if(orig == NULL)
    {
        *copy = NULL;
        return FWKNOPD_SUCCESS;
    }

    return create_connection_item( orig->connection_id,
                                   orig->sdp_id,
                                   orig->src_ip_str,
                                   orig->src_port,
                                   orig->dst_ip_str,
                                   orig->dst_port,
                                   orig->start_time,
                                   orig->end_time,
                                   copy );
}


static int duplicate_connection_list(connection_t orig, connection_t *copy)
{
    int rv = FWKNOPD_SUCCESS;
    connection_t this_conn = orig;
    connection_t last_conn = NULL;
    connection_t new_list = NULL;

    if(this_conn == NULL)
    {
        *copy = NULL;
        return rv;
    }

    if((rv = duplicate_connection_item(this_conn, &new_list)) != FWKNOPD_SUCCESS)
        return rv;

    last_conn = new_list;

    while(this_conn->next != NULL)
    {
        this_conn = this_conn->next;

        if((rv = duplicate_connection_item(this_conn, &(last_conn->next))) != FWKNOPD_SUCCESS)
            goto cleanup;

        // shouldn't be possible, but just to be safe
        if(last_conn->next == NULL)
        {
            log_msg(LOG_ERR, "duplicate_connection_list() duplicate_connection_item "
                    "failed to set last_conn->next.");
            rv = FWKNOPD_ERROR_CONNTRACK;
            goto cleanup;
        }

        last_conn = last_conn->next;
    }

    *copy = new_list;
    return rv;

cleanup:
    destroy_connection_list(new_list);
    *copy = NULL;
    return rv;
}


static int store_in_connection_hash_tbl(hash_table_t *tbl, connection_t this_conn)
{
    int res = FWKNOPD_SUCCESS;
    bstring key = NULL;
    char id_str[SDP_MAX_CLIENT_ID_STR_LEN + 1] = {0};
    connection_t present_conns = NULL;

    // convert the sdp id integer to a bstring
    snprintf(id_str, SDP_MAX_CLIENT_ID_STR_LEN, "%"PRIu32, this_conn->sdp_id);
    key = bfromcstr(id_str);
    // key is not freed if hash_table_set is called,
    // because the hash table keeps it

    // if a node for this SDP ID doesn't yet exist in the table
    // gotta make it
    if( (present_conns = hash_table_get(tbl, key)) == NULL)
    {
        log_msg(LOG_DEBUG, "store_in_connection_hash_tbl() ID %"PRIu32
                " not yet in table. \n", this_conn->sdp_id);

        if( (res = hash_table_set(tbl, key, this_conn)) != FWKNOPD_SUCCESS)
        {
            log_msg(LOG_ERR,
                "[*] Fatal memory allocation error updating 'latest' connection tracking hash table"
            );
            bdestroy(key);
        }
    }
    else
    {
        log_msg(LOG_DEBUG, "store_in_connection_hash_tbl() ID %"PRIu32
                " already exists in table. \n", this_conn->sdp_id);

        // key is no longer needed in this case, didn't create a new hash node
        bdestroy(key);

        // this one should be impossible to fail, but we will still return the res
        res = add_to_connection_list(&present_conns, this_conn);

        log_msg(LOG_DEBUG, "store_in_connection_hash_tbl() Added conn to current "
        		"list for SDP ID: %"PRIu32" \n", this_conn->sdp_id);
    }

    return res;
}


static int check_conntrack(fko_srv_options_t *opts, int *conn_count_r)
{
    int    conn_count = 0, res = FWKNOPD_SUCCESS;
    connection_t this_conn = NULL;
    connection_t next = NULL;

    log_msg(LOG_DEBUG, "check_conntrack() Getting latest connections... \n");

    if( (res = search_conntrack(opts, NULL, &this_conn, &conn_count)) != FWKNOPD_SUCCESS)
        return res;

    if(verbosity >= LOG_DEBUG)
    {
		log_msg(LOG_DEBUG, "\n\nDumping connection list from search_conntrack:");
		print_connection_list(this_conn);
    }

    while(this_conn != NULL)
    {
    	next = this_conn->next;
    	this_conn->next = NULL;

        if( (res = store_in_connection_hash_tbl(latest_connection_hash_tbl, this_conn)) != FWKNOPD_SUCCESS)
        {
            // destroy remainder of list,
            // not ones that were successfully stored in the hash table
        	destroy_connection_item(this_conn);
            destroy_connection_list(next);
            return res;
        }

        log_msg(LOG_DEBUG, "check_conntrack() back from storing a conn in latest_connection_hash_tbl \n");

        this_conn = next;
    }

    *conn_count_r = conn_count;

    return res;
}


static void destroy_hash_node_cb(hash_table_node_t *node)
{
  if(node->key != NULL) bdestroy((bstring)(node->key));
  if(node->data != NULL)
  {
      // this function takes care of all connection nodes (NOT hash table nodes)
      // for this SDP ID, including the very first one
      destroy_connection_list((connection_t)(node->data));
  }
}


static int connection_items_match(connection_t a, connection_t b)
{
    // make sure neither is NULL first
    if(!(a && b))
        return 0;

    if(    a->sdp_id   == b->sdp_id     &&
        a->src_port == b->src_port   &&
        a->dst_port == b->dst_port   &&
        strncmp(a->src_ip_str, b->src_ip_str, MAX_IPV4_STR_LEN) == 0 &&
        strncmp(a->dst_ip_str, b->dst_ip_str, MAX_IPV4_STR_LEN) == 0)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}


static int compare_connection_lists(connection_t *known_conns,
                                    connection_t *current_conns,
                                    connection_t *closed_conns)
{
    int rv = FWKNOPD_SUCCESS;
    int match = 0;
    connection_t this_known_conn = *known_conns;
    connection_t prev_known_conn = NULL;
    connection_t next_conn = NULL;
    connection_t this_current_conn = NULL;
    connection_t prev_current_conn = NULL;
    connection_t new_conns = NULL;
    connection_t temp_conn = NULL;

    log_msg(LOG_DEBUG, "compare_connection_lists() entered");

    while(this_known_conn != NULL)
    {
        match = 0;
        this_current_conn = *current_conns;
        prev_current_conn = NULL;

        while(this_current_conn != NULL)
        {
            if(    (match = connection_items_match(this_known_conn, this_current_conn)) == 1 )
            {
                // got a match, remove from current_conns list

                 // if very first connection item was the match
                if(prev_current_conn == NULL)
                {
                    *current_conns = this_current_conn->next;
                }
                else
                {
                    prev_current_conn->next = this_current_conn->next;
                }

                destroy_connection_item(this_current_conn);
                break;
            }

            prev_current_conn = this_current_conn;
            this_current_conn = this_current_conn->next;

        }   // END while(this_current_conn != NULL)

        next_conn = this_known_conn->next;

        // if a match was found for this known connection
        if(match)
        {
            // then it's still live, so
            // the conn stays in the known conn list
            // just move to next known conn
            prev_known_conn = this_known_conn;
        }
        else
        {
            // no match was found for this known connection
            // the conn no longer exists, move from known conns to closed conns
            // when removing a known conn, prev_known_conn should not be updated
            this_known_conn->next = NULL;

            log_msg(LOG_DEBUG, "compare_connection_lists() adding previously known conn to closed list");

            if( (rv = add_to_connection_list(closed_conns, this_known_conn))
                        != FWKNOPD_SUCCESS)
            {
                goto cleanup;
            }

            // if we're removing the first conn in the known conn list
            if(prev_known_conn == NULL)
            {
                *known_conns = next_conn;
            }
            else
            {
                prev_known_conn->next = next_conn;
            }

        }  // END if(match)

        // move to next known conn
        this_known_conn = next_conn;

    }  // END while(this_known_conn != NULL)

    // any remaining conns in current_conns list are totally new
    // add copies to known_conns
    if(*current_conns != NULL)
    {
        log_msg(LOG_DEBUG, "compare_connection_lists() adding remaining latest "
                "conns to known conns");

        if( (rv = duplicate_connection_list(*current_conns, &new_conns)) != FWKNOPD_SUCCESS)
        {
            goto cleanup;
        }

        temp_conn = new_conns;
        while(temp_conn != NULL)
        {
            last_conn_id++;
            temp_conn->connection_id = last_conn_id;
            temp_conn = temp_conn->next;
        }

        if( (rv = add_to_connection_list(known_conns, new_conns))
                != FWKNOPD_SUCCESS)
        {
            goto cleanup;
        }

    }

    return rv;

cleanup:
    destroy_connection_list(new_conns);
    destroy_connection_list(*closed_conns);
    *closed_conns = NULL;

    return rv;
}


static int traverse_print_conn_items_cb(hash_table_node_t *node, void *arg)
{
    print_connection_list((connection_t)(node->data));

    return FWKNOPD_SUCCESS;
}


static int traverse_compare_latest_cb(hash_table_node_t *node, void *arg)
{
    int rv = FWKNOPD_SUCCESS;
    connection_t known_conns = NULL;
    connection_t current_conns = NULL;
    connection_t copy_current_conns = NULL;
    time_t *end_time = (time_t*)arg;
    connection_t closed_conns = NULL;
    connection_t temp_conn = NULL;
    bstring key = (bstring)node->key;

    // just a safety check, shouldn't be possible
    if(node->data == NULL)
    {
        hash_table_delete(connection_hash_tbl, key);
        return rv;
    }

    // going to manipulate this data directly and reset the data pointer
    known_conns = (connection_t)(node->data);

    // check whether this SDP ID still has any current connections
    if( (current_conns = hash_table_get(latest_connection_hash_tbl, key)) == NULL)
    {
        // update each conn item with the closing time
    	temp_conn = known_conns;
        while(temp_conn != NULL)
        {
        	temp_conn->end_time = *end_time;
        	temp_conn = temp_conn->next;
        }

        log_msg(LOG_WARNING, "All connections closed for SDP ID %"PRIu32":",
				known_conns->sdp_id);
        print_connection_list(known_conns);


        // add these closed connections to the ctrl message list
        if( (rv = add_to_connection_list(&msg_conn_list, known_conns))
                != FWKNOPD_SUCCESS)
        {
            goto cleanup;
        }

        // set node->data to NULL so the list is not deleted with the node
        node->data = NULL;

        // this SDP ID no longer has connections, remove entirely from
        // known connection list, hash table traverser is fine with
        // deleting random nodes along the way
        hash_table_delete(connection_hash_tbl, key);
        return FWKNOPD_SUCCESS;
    }

    // at this point, we know this ID has both known and current connections
    // have to compare each connection in-depth

    // first make a copy of current_conns to avoid data corruption
    // because we'll be deleting the node from 'latest' conn hash table
    if((rv = duplicate_connection_list(current_conns, &copy_current_conns)) != FWKNOPD_SUCCESS)
        return rv;

    // delete the entry in 'latest' conn hash table, we'll take it from here
    hash_table_delete(latest_connection_hash_tbl, key);

    // following function updates known_conns with both new and still-running conns
    // what will be left in copy_current_conns is a copy of all new conns for
    // informing ctrl, closed_conns will obviously be those shut down
    if( (rv = compare_connection_lists(&known_conns, &copy_current_conns,
            &closed_conns)) != FWKNOPD_SUCCESS)
    {
        goto cleanup;
    }

    if(known_conns == NULL)
    {
        // shouldn't be possible, got here because there were current conns to analyze
        log_msg(LOG_ERR, "traverse_compare_latest_cb() ERROR: comparison of "
                "known and current connections resulted in zero known "
                "connections for ID %s", bdata(key));
        goto cleanup;
    }
    else
    {
        node->data = known_conns;
    }

    // add opened conns to the ctrl message list
    if(copy_current_conns != NULL)
    {
        log_msg(LOG_WARNING, "New connections from SDP ID %"PRIu32":",
        		copy_current_conns->sdp_id);
        print_connection_list(copy_current_conns);

        if( (rv = add_to_connection_list(&msg_conn_list, copy_current_conns))
                != FWKNOPD_SUCCESS)
        {
            goto cleanup;
        }
    }
    copy_current_conns = NULL;

    // add closed conns to the ctrl message list
    if(closed_conns != NULL)
    {
        // update each conn item with the closing time
    	temp_conn = closed_conns;
        while(temp_conn != NULL)
        {
        	temp_conn->end_time = *end_time;
        	temp_conn = temp_conn->next;
        }

        log_msg(LOG_WARNING, "Following connections closed for SDP ID %"PRIu32":",
				closed_conns->sdp_id);
        print_connection_list(closed_conns);

        if( (rv = add_to_connection_list(&msg_conn_list, closed_conns))
                != FWKNOPD_SUCCESS)
        {
            goto cleanup;
        }

    }

    return rv;

cleanup:
    destroy_connection_list(copy_current_conns);
    destroy_connection_list(closed_conns);
    return rv;
}


static int traverse_move_latest_to_known_cb(hash_table_node_t *node, void *arg)
{
    int rv = FWKNOPD_SUCCESS;
    bstring key = NULL;
    connection_t temp_conn = NULL;

    log_msg(LOG_DEBUG, "traverse_move_latest_to_known_cb() entered");

    if(node->data == NULL)
    {
        log_msg(LOG_ERR, "traverse_move_latest_to_known_cb() node->data is NULL, shouldn't happen\n");
        hash_table_delete(latest_connection_hash_tbl, node->key);
        return rv;
    }

    temp_conn = (connection_t)(node->data);

    log_msg(LOG_WARNING, "New connections from SDP ID %"PRIu32":",
    		temp_conn->sdp_id);
    print_connection_list(temp_conn);

    // assign connection ids
    while(temp_conn != NULL)
    {
        last_conn_id++;
        temp_conn->connection_id = last_conn_id;
        temp_conn = temp_conn->next;
    }

    if((key = bstrcpy((bstring)(node->key))) == NULL)
    {
        log_msg(LOG_ERR, "traverse_move_latest_to_known_cb() Failed to duplicate key");
        return FWKNOPD_ERROR_MEMORY_ALLOCATION;
    }

    if((rv = duplicate_connection_list((connection_t)(node->data), &temp_conn)) != FWKNOPD_SUCCESS)
    {
        bdestroy(key);
        goto cleanup;
    }

    // copy all remaining items to known conns hash table
    if( (rv = hash_table_set(connection_hash_tbl, (void*)key, temp_conn)) != FWKNOPD_SUCCESS)
    {
        bdestroy(key);
        goto cleanup;
    }

    temp_conn = NULL;

    // also copy all remaining items to msg list for ctrl
    if((rv = duplicate_connection_list((connection_t)(node->data), &temp_conn)) != FWKNOPD_SUCCESS)
        goto cleanup;

    log_msg(LOG_DEBUG, "traverse_move_latest_to_known_cb() adding new conns to msg list\n");

    if( (rv = add_to_connection_list(&msg_conn_list, temp_conn)) != FWKNOPD_SUCCESS)
        goto cleanup;

    hash_table_delete(latest_connection_hash_tbl, node->key);
    return rv;

cleanup:
    destroy_connection_list(temp_conn);
    return rv;
}


static int traverse_validate_connections_cb(hash_table_node_t *node, void *arg)
{
    int rv = FWKNOPD_SUCCESS;
    fko_srv_options_t *opts = (fko_srv_options_t*)arg;
    acc_stanza_t *acc = NULL;
    bstring key = (bstring)node->key;
    connection_t this_conn = (connection_t)(node->data);
    connection_t prev_conn = NULL;
    connection_t next_conn = NULL;
    connection_t temp_conn = NULL;
    int conn_valid = 0;
    char criteria[CRITERIA_BUF_LEN];
    time_t now = time(NULL);

    // always double-check
    if(this_conn == NULL)
    {
        hash_table_delete(connection_hash_tbl, key);
        return rv;
    }

    memset(criteria, 0x0, CRITERIA_BUF_LEN);

    // see if sdp id still exists in access table
    if( (acc = hash_table_get(opts->acc_stanza_hash_tbl, key)) == NULL)
    {
        // this sdp id is no longer authorized to access anything
    	// remove all connections marked with this sdp id
    	snprintf(criteria, CRITERIA_BUF_LEN-1, "-m %"PRIu32, this_conn->sdp_id);

        if( (rv = close_connections(opts, criteria)) != FWKNOPD_SUCCESS)
        {
            return rv;
        }

        // set the end time for all of the connections
        temp_conn = this_conn;
		while(temp_conn != NULL)
		{
			temp_conn->end_time = now;
			temp_conn = temp_conn->next;
		}


        // print the closed conns
        log_msg(LOG_WARNING, "Gateway closed the following (i.e. all) connections from SDP ID %"PRIu32":",
        		this_conn->sdp_id);
        print_connection_list(this_conn);

		// pin the whole list onto the ctrl message list
		if( (rv = add_to_connection_list(&msg_conn_list, node->data)) != FWKNOPD_SUCCESS)
		{
			return rv;
		}

		// make sure the hash table node no longer points to the
		// connection list
        node->data = NULL;
        this_conn = NULL;
    }

    while(this_conn != NULL)
    {
        conn_valid = 0;
        if( (rv = validate_connection(acc, this_conn, &conn_valid)) != FWKNOPD_SUCCESS)
            return rv;

        next_conn = this_conn->next;

        if(conn_valid == 1)
        {
            prev_conn = this_conn;
        }
        else
        {
            // remove from node->data list
            if(prev_conn == NULL)
                node->data = this_conn->next;
            else
                prev_conn->next = this_conn->next;

            this_conn->next = NULL;

        	// set the closing time
        	this_conn->end_time = now;

            // create search criteria to close the connection
        	snprintf(criteria, CRITERIA_BUF_LEN-1, CONNMARK_SEARCH_ARGS,
        			 this_conn->sdp_id, this_conn->src_ip_str, this_conn->src_port,
					 this_conn->dst_ip_str, this_conn->dst_port);

        	// close it
            if( (rv = close_connections(opts, criteria)) != FWKNOPD_SUCCESS)
            {
                return rv;
            }

            // print closed connection
            log_msg(LOG_WARNING, "Gateway closed the following connection from SDP ID %"PRIu32":",
            		this_conn->sdp_id);
            print_connection_list(this_conn);

            // add to the ctrl msg list
    		if( (rv = add_to_connection_list(&msg_conn_list, this_conn)) != FWKNOPD_SUCCESS)
    		{
    			return rv;
    		}
        }

        this_conn = next_conn;
    }

    // if it happens that no connections are left open
    // delete the node from the known connections hash table
    if(node->data == NULL)
        hash_table_delete(connection_hash_tbl, key);

    return FWKNOPD_SUCCESS;
}

static int conn_id_file_check(const char *file, int *exists)
{
    struct stat st;
    uid_t caller_uid = 0;

    // if file exists
    if((stat(file, &st)) == 0)
    {
        *exists = 1;

        // Make sure it is a regular file
        if(S_ISREG(st.st_mode) != 1 && S_ISLNK(st.st_mode) != 1)
        {
            log_msg(LOG_WARNING,
                "[-] file: %s is not a regular file or symbolic link.",
                file
            );
            return FWKNOPD_ERROR_CONNTRACK;
        }

        if((st.st_mode & (S_IRWXU|S_IRWXG|S_IRWXO)) != (S_IRUSR|S_IWUSR))
        {
            log_msg(LOG_WARNING,
                "[-] file: %s permissions should only be user read/write (0600, -rw-------)",
                file
            );
        }

        caller_uid = getuid();
        if(st.st_uid != caller_uid)
        {
            log_msg(LOG_WARNING, "[-] file: %s (owner: %llu) not owned by current effective user id: %llu",
                file, (unsigned long long)st.st_uid, (unsigned long long)caller_uid);
        }
    }
    else
    {
        // if the path doesn't exist, just return, but otherwise something
        // went wrong
        if(errno != ENOENT)
        {
            log_msg(LOG_ERR, "[-] stat() against file: %s returned: %s",
                file, strerror(errno));
            return FWKNOPD_ERROR_CONNTRACK;
        }

        *exists = 0;
    }

    return FWKNOPD_SUCCESS;
}


static void store_last_conn_id(const fko_srv_options_t *opts)
{
    int     op_fd, num_bytes = 0;
    char    buf[CONN_ID_BUF_LEN] = {0};

    // Don't store it if it's zero
    if(last_conn_id == 0)
        return;

    // Reset errno (just in case)
    errno = 0;

    // Open the PID file
    op_fd = open(
        opts->config[CONF_CONN_ID_FILE], O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR
    );

    if(op_fd == -1)
    {
        perror("Error trying to open connection ID file: ");
        return;
    }

    if(fcntl(op_fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        close(op_fd);
        perror("Unexpected error from fcntl: ");
        return;
    }

    // Write last connection ID to the file
    snprintf(buf, CONN_ID_BUF_LEN, "%"PRIu64"\n", last_conn_id);

    log_msg(LOG_DEBUG, "[+] Writing last connection ID (%"PRIu64") to the lock file: %s",
        last_conn_id, opts->config[CONF_CONN_ID_FILE]);

    num_bytes = write(op_fd, buf, strlen(buf));

    if(errno || num_bytes != strlen(buf))
        perror("Connection ID file write error: ");

    // Sync/flush regardless...
    fsync(op_fd);

    close(op_fd);

    return;
}

static int get_set_last_conn_id(const fko_srv_options_t *opts)
{
    int rv = FWKNOPD_SUCCESS;
    int exists = 0;
    int     op_fd, bytes_read = 0;
    char    buf[CONN_ID_BUF_LEN] = {0};
    uint64_t conn_id            = 0;

    log_msg(LOG_DEBUG, "get_set_last_conn_id() checking file perms...");
    if( (rv = conn_id_file_check(opts->config[CONF_CONN_ID_FILE], &exists)) != FWKNOPD_SUCCESS)
    {
        log_msg(LOG_ERR, "conn_id_file_check() error\n");
        return(rv);
    }

    if(!exists)
    {
        log_msg(LOG_WARNING, "get_set_last_conn_id() conn ID file does not yet exist, starting at zero");
        last_conn_id = 0;
        return FWKNOPD_SUCCESS;
    }

    log_msg(LOG_DEBUG, "get_set_last_conn_id() opening the file...");
    op_fd = open(opts->config[CONF_CONN_ID_FILE], O_RDONLY);

    if(op_fd == -1)
    {
        log_msg(LOG_ERR, "get_set_last_conn_id() ERROR - conn ID file exists but can't open");
        last_conn_id = 0;
        return FWKNOPD_ERROR_CONNTRACK;
    }

    log_msg(LOG_DEBUG, "get_set_last_conn_id() reading the file...");
    bytes_read = read(op_fd, buf, CONN_ID_BUF_LEN);
    if (bytes_read > 0)
    {
        buf[CONN_ID_BUF_LEN-1] = '\0';

        log_msg(LOG_DEBUG, "get_set_last_conn_id() Got following string from the conn ID file: %s\n",
                buf);

        conn_id = strtoull_wrapper(buf, 0, UINT64_MAX, NO_EXIT_UPON_ERR, &rv);
        if(rv != FKO_SUCCESS)
        {
            log_msg(LOG_ERR, "get_set_last_conn_id() ERROR converting conn ID "
                    "string to uint64_t");
        }
        else
        {
            last_conn_id = conn_id;
            log_msg(LOG_DEBUG, "get_set_last_conn_id() setting conn ID value: %"PRIu64"\n",
                    last_conn_id);
        }
    }
    else if (bytes_read < 0)
    {
        rv = FWKNOPD_ERROR_CONNTRACK;
        perror("Error trying to read() PID file: ");
    }

    close(op_fd);

    return rv;
}



static int init_connection_tracking(fko_srv_options_t *opts)
{
    int hash_table_len = 0;
    int is_err = FWKNOPD_SUCCESS;

    verbosity = LOG_DEFAULT_VERBOSITY + opts->verbose;

    // set the global connection ID variable
    if( (is_err = get_set_last_conn_id(opts)) != FWKNOPD_SUCCESS)
        return is_err;

    // connection table should be same length as access stanza hash table
    hash_table_len = strtol_wrapper(opts->config[CONF_ACC_STANZA_HASH_TABLE_LENGTH],
                           MIN_ACC_STANZA_HASH_TABLE_LENGTH,
                           MAX_ACC_STANZA_HASH_TABLE_LENGTH,
                           NO_EXIT_UPON_ERR,
                           &is_err);

    if(is_err != FKO_SUCCESS)
    {
        log_msg(LOG_ERR, "[*] var %s value '%s' not in the range %d-%d",
                "ACC_STANZA_HASH_TABLE_LENGTH",
                opts->config[CONF_ACC_STANZA_HASH_TABLE_LENGTH],
                MIN_ACC_STANZA_HASH_TABLE_LENGTH,
                MAX_ACC_STANZA_HASH_TABLE_LENGTH);
        clean_exit(opts, NO_FW_CLEANUP, EXIT_FAILURE);
    }

    connection_hash_tbl = hash_table_create(hash_table_len,
            NULL, NULL, destroy_hash_node_cb);

    if(connection_hash_tbl == NULL)
    {
        log_msg(LOG_ERR,
            "[*] Fatal memory allocation error creating connection tracking hash table"
        );
        clean_exit(opts, NO_FW_CLEANUP, EXIT_FAILURE);
    }

    latest_connection_hash_tbl = hash_table_create(hash_table_len,
            NULL, NULL, destroy_hash_node_cb);

    if(latest_connection_hash_tbl == NULL)
    {
        log_msg(LOG_ERR,
            "[*] Fatal memory allocation error creating 'latest' connection tracking hash table"
        );
        clean_exit(opts, NO_FW_CLEANUP, EXIT_FAILURE);
    }

    return is_err;
}

void destroy_connection_tracker(fko_srv_options_t *opts)
{
    store_last_conn_id(opts);

    if(connection_hash_tbl != NULL)
    {
        hash_table_destroy(connection_hash_tbl);
        connection_hash_tbl = NULL;
    }

    if(latest_connection_hash_tbl != NULL)
    {
        hash_table_destroy(latest_connection_hash_tbl);
        latest_connection_hash_tbl = NULL;
    }

    if(msg_conn_list != NULL)
    {
        destroy_connection_list(msg_conn_list);
        msg_conn_list = NULL;
    }
}

int update_connections(fko_srv_options_t *opts)
{
    int res = FWKNOPD_SUCCESS;
    int pres_conn_count = 0;
    time_t now = 0;

    // if it's first time, init the conn tracking table
    if(connection_hash_tbl == NULL)
    {
        if( (res = init_connection_tracking(opts)) != FWKNOPD_SUCCESS)
        {
            log_msg(LOG_ERR,
                "[*] Failed to initialize connection tracking."
            );
            return res;
        }
    }

    // first get list of current connections
    if( (res = check_conntrack(opts, &pres_conn_count)) != FWKNOPD_SUCCESS)
    {
        // pass errors up
        return res;
    }

    if(verbosity >= LOG_DEBUG)
    {
        log_msg(LOG_DEBUG, "After check_conntrack, dumping hash table "
                "of current (i.e. latest) connection items:");
        hash_table_traverse(latest_connection_hash_tbl, traverse_print_conn_items_cb, NULL);
        log_msg(LOG_DEBUG, "\n\n");
    }

    now = time(NULL);

    // walk list of known connections
    if( hash_table_traverse(connection_hash_tbl, traverse_compare_latest_cb, &now)  != FWKNOPD_SUCCESS )
    {
        return FWKNOPD_ERROR_CONNTRACK;
    }

    // what's left in 'latest' conns are new, unknown conns
    // add to known list and to report for ctrl
    if( hash_table_traverse(latest_connection_hash_tbl, traverse_move_latest_to_known_cb, NULL)  != FWKNOPD_SUCCESS )
    {
        return FWKNOPD_ERROR_CONNTRACK;
    }

    if(verbosity >= LOG_DEBUG)
    {
        log_msg(LOG_DEBUG, "Finished updating all connections");

        log_msg(LOG_DEBUG, "Dumping known connections hash table:");
        hash_table_traverse(connection_hash_tbl, traverse_print_conn_items_cb, NULL);

        log_msg(LOG_DEBUG, "\n\nDumping current connections hash table (should now be empty):");
        hash_table_traverse(latest_connection_hash_tbl, traverse_print_conn_items_cb, NULL);

        log_msg(LOG_DEBUG, "\n\nDumping message list for controller:");
        print_connection_list(msg_conn_list);

        log_msg(LOG_DEBUG, "\n\n");
    }

    return FWKNOPD_SUCCESS;
}

int validate_connections(fko_srv_options_t *opts)
{
    return hash_table_traverse(connection_hash_tbl, traverse_validate_connections_cb, opts);
}


static int make_json_from_conn_item(connection_t conn, json_object **jconn_r)
{
    json_object *jconn = json_object_new_object();

    json_object_object_add(jconn, "connection_id", json_object_new_int64(conn->connection_id));
    json_object_object_add(jconn, "sdp_id", json_object_new_int(conn->sdp_id));
    json_object_object_add(jconn, "source_ip", json_object_new_string(conn->src_ip_str));
    json_object_object_add(jconn, "source_port", json_object_new_int(conn->src_port));
    json_object_object_add(jconn, "destination_ip", json_object_new_string(conn->dst_ip_str));
    json_object_object_add(jconn, "destination_port", json_object_new_int(conn->dst_port));
    json_object_object_add(jconn, "start_timestamp", json_object_new_int64(conn->start_time));
    json_object_object_add(jconn, "end_timestamp", json_object_new_int64(conn->end_time));

    *jconn_r = jconn;
    return FWKNOPD_SUCCESS;
}

static int send_connection_report(fko_srv_options_t *opts)
{
    int rv = FWKNOPD_SUCCESS;
    json_object *jarray = NULL;
    json_object *jconn = NULL;
    connection_t this_conn = msg_conn_list;

    if(msg_conn_list == NULL)
        return rv;

    if(verbosity >= LOG_DEBUG)
    {
		log_msg(LOG_DEBUG, "\n\nDumping message list for controller:");
		print_connection_list(msg_conn_list);
    }

    jarray = json_object_new_array();

    while(this_conn != NULL)
    {
        if( (rv = make_json_from_conn_item(this_conn, &jconn)) != FWKNOPD_SUCCESS)
        {
            goto cleanup;
        }
        json_object_array_add(jarray, jconn);
        this_conn = this_conn->next;
    }

    rv = sdp_ctrl_client_send_message(opts->ctrl_client, "connection_update", jarray);

cleanup:
    if(jarray != NULL && json_object_get_type(jarray) != json_type_null)
    {
        json_object_put(jarray);
        jarray = NULL;
    }

    return rv;
}


int consider_reporting_connections(fko_srv_options_t *opts)
{
    int rv = FWKNOPD_SUCCESS;
    time_t now = time(NULL);
    int interval = strtol_wrapper(opts->config[CONF_CONN_REPORT_INTERVAL], 1,
            INT32_MAX, NO_EXIT_UPON_ERR, &rv);

    if(rv != FKO_SUCCESS)
    {
        log_msg(LOG_ERR, "consider_reporting_connections() ERROR retrieving "
                "reporting interval from server config.");
        return rv;
    }

    // if it's not time, just return success
    if(now < next_ctrl_msg_due)
        return rv;

    // if nothing new to report, just return success
    if(msg_conn_list == NULL)
        return rv;

    // time to send

    if(verbosity >= LOG_DEBUG)
    {
        log_msg(LOG_DEBUG, "Time to send connection update");

        log_msg(LOG_DEBUG, "Dumping known connections hash table:");
        hash_table_traverse(connection_hash_tbl, traverse_print_conn_items_cb, NULL);

        log_msg(LOG_DEBUG, "\n\nDumping message list for controller:");
        print_connection_list(msg_conn_list);

        log_msg(LOG_DEBUG, "\n\n");
    }

    // send message
    if( (rv = send_connection_report(opts)) != FWKNOPD_SUCCESS)
        return rv;

    // free message list
    destroy_connection_list(msg_conn_list);
    msg_conn_list = NULL;

    // update next_ctrl_msg_due
    next_ctrl_msg_due = now + interval;

    if(next_ctrl_msg_due < now)
    {
        log_msg(LOG_ERR, "consider_reporting_connections() variable next_ctrl_msg_due "
                "has overflowed, possibly due to a large report interval. Report "
                "interval is %d seconds.", interval);
        return FWKNOPD_ERROR_CONNTRACK;
    }

    return rv;
}
//#endif
