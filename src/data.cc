/*  =========================================================================
    data - composite metrics data structure

    Copyright (C) 2014 - 2015 Eaton

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    =========================================================================
*/

/*
@header
    data - composite metrics data structure, holds information about known
           assets, configures sensors, stores information about metrics
           that would be produced by the actual configuration.
@discuss
@end
*/

#include "composite_metrics_classes.h"
#include <set>
#include <string>

//  Structure of our class
struct _data_t {
    // Information about all interesting assets for this agent
    zhashx_t *all_assets; // asset_name -> its message definition. Owns messages
    // Structure of sensors
    zhashx_t *last_configuration; // asset_name -> list of sensors (each sensor is represented as message). Doesn't own messages
    bool is_reconfig_needed; // indicates, if recently added asset can change configuration
    std::set<std::string> produced_metrics; // list of metrics, that are now produced by composite_metric
};

//  --------------------------------------------------------------------------
//  Create a new data

data_t *
data_new (void)
{
    data_t *self = (data_t *) zmalloc (sizeof (data_t));
    if ( self ) {
        self->produced_metrics = {};
        self->last_configuration = zhashx_new ();
        if ( self->last_configuration ) {
            self->all_assets = zhashx_new ();
        }
        if ( self->all_assets ) {
            zhashx_set_destructor (self->last_configuration, (zhashx_destructor_fn *) zlistx_destroy);
            zhashx_set_destructor (self->all_assets, (zhashx_destructor_fn *) bios_proto_destroy);
            self->is_reconfig_needed = false;
        }
        else {
            data_destroy (&self);
        }
    }
    return self;
}

//  --------------------------------------------------------------------------
//  Get a list of sensors assigned to the specified asset, if for this asset
//  there is no any record, creates empty one and returns it.

static zlistx_t*
s_get_assigned_sensors (data_t *self, const char *asset_name)
{
    zlistx_t *already_assigned_sensors = (zlistx_t *) zhashx_lookup (self->last_configuration, asset_name);
    // if no sensors were assigned yet, create empty list
    if ( already_assigned_sensors == NULL ) {
        already_assigned_sensors = zlistx_new ();
        // TODO: handle NULL-pointer
        zhashx_insert (self->last_configuration, asset_name, (void *) already_assigned_sensors);
        // ATTENTION: zhashx_set_destructor is not called intentionally, as here we have only "links" on
        // sensors, because "all_assets" owns this information!
    }
    return already_assigned_sensors;
}

//  --------------------------------------------------------------------------
//  According known information about assets, decide, where sensors logically belong to

void
data_reassign_sensors (data_t *self)
{
    assert (self);
    // delete old configuration first
    zhashx_purge (self->last_configuration);
    // explicitly say, that we suppose, that no further reconfiguration is neened
    self->is_reconfig_needed = false;

    // go through every known sensor (if it is not sensor, skip it)
    zlistx_t *asset_names = data_asset_names (self);

    char *one_sensor_name = (char *) zlistx_first (asset_names);
    while (one_sensor_name) {
        // get an asset
        bios_proto_t *one_sensor = (bios_proto_t *) zhashx_lookup (self->all_assets, one_sensor_name);
        // discover the sub-type of the asset
        const char *subtype = bios_proto_aux_string (one_sensor, "subtype", "");
        // check if it is sensor or not
        if ( !streq (subtype, "sensor") ) {
            // if it is NOT sensor -> do nothing!
            // and we can move to next one
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }
        // if it is sensor, do CONFIGURATION

        // first of all, take its logical asset (it is supposed to be NOT empty!!!)
        // because if it would have been empty it would not be placed in this map
        const char *logical_asset_name = bios_proto_ext_string (one_sensor, "logical_asset", "");

        // BIOS-2484: start preparation for further usage
        // find detailed information about logical asset
        bios_proto_t *logical_asset = (bios_proto_t *) zhashx_lookup (self->all_assets, logical_asset_name);
        if ( logical_asset == NULL ) {
            log_warning ("Inconsistecy (yet): detailes about logical asset '%s' are not known -> skip sensor '%s'", logical_asset_name, one_sensor_name);
            // Detailed information about logical asset was not found
            // It can happen if:
            //  * reconfiguration started before detailed "logical_asset" message arrived
            //  * something is really wrong!
            self->is_reconfig_needed = true;
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }
        // BIOS-2484: end

        // BIOS-2484: start - ignore sensors assigned to the NON-RACK asset
        const char *logical_asset_type = bios_proto_aux_string (logical_asset, "type", "");
        if ( !streq (logical_asset_type, "rack") ) {
            log_warning ("Sensor '%s' assigned to non-'rack' is ignored", one_sensor_name);
            one_sensor_name = (char *) zlistx_next (asset_names);
            continue;
        }
        // BIOS-2484: end

        // So, now let us put our sensor to the right place

        // Find already assigned sensors
        zlistx_t *already_assigned_sensors = s_get_assigned_sensors (self, logical_asset_name);
        // add sensor to the list
        zlistx_add_end (already_assigned_sensors, (void *) one_sensor);

        // BIOS-2484: start - propagate sensor in physical topology
        // (need to add sensor to all "parents" of the logical asset)
        // Actually, the chain is: dc-room-row-rack-device-device -> max 5 level parents can be
        // But here, we start from rack -> only 3 level is available at maximum
        const char *l_parent_name = bios_proto_aux_string (logical_asset, "parent_name.1", NULL);
        if ( l_parent_name ) {
            already_assigned_sensors = s_get_assigned_sensors (self, l_parent_name);
            zlistx_add_end (already_assigned_sensors, (void *) one_sensor);
        }

        l_parent_name = bios_proto_aux_string (logical_asset, "parent_name.2", NULL);
        if ( l_parent_name ) {
            already_assigned_sensors = s_get_assigned_sensors (self, l_parent_name);
            zlistx_add_end (already_assigned_sensors, (void *) one_sensor);
        }

        l_parent_name = bios_proto_aux_string (logical_asset, "parent_name.3", NULL);
        if ( l_parent_name ) {
            already_assigned_sensors = s_get_assigned_sensors (self, l_parent_name);
            zlistx_add_end (already_assigned_sensors, (void *) one_sensor);
        }
        // BIOS-2484: end

        // at this point configuration of this sensor is done
        // and we can move to next one
        one_sensor_name = (char *) zlistx_next (asset_names);
    }
    zlistx_destroy (&asset_names);
}

//  --------------------------------------------------------------------------
//  Before using this functionality, sensors should be assigned to the right positions
//  by calling 'data_reassign_sensors' function.
//  Get list of sensors assigned to the asset
//  You can limit the list of sensors returned to a certain 'sensor_function',
//  NULL returns all sensors.
//  Returns NULL when for 'asset_name' T&H sensors are not known or asset_name is not known at all
//  or in case of memory issues
//  The caller is responsible for destroying the return value when finished with it

zlistx_t *
data_get_assigned_sensors (
        data_t *self,
        const char *asset_name,
        const char *sensor_function)
{
    assert (self);
    assert (asset_name);

    zlistx_t *sensors = (zlistx_t *) zhashx_lookup (self->last_configuration, asset_name);
    if (!sensors) {
        log_info (
                "Asset '%s' has no sensors assigned -> no T&H would be computed",
                asset_name);
        return NULL;
    }
    zlistx_t *return_sensor_list = zlistx_new ();
    if ( !return_sensor_list ) {
        log_error ("Memory allocation error");
        return NULL;
    }
    zlistx_set_destructor (return_sensor_list, (czmq_destructor *) bios_proto_destroy);
    zlistx_set_duplicator (return_sensor_list, (czmq_duplicator *) bios_proto_dup);

    bios_proto_t *one_sensor = (bios_proto_t *) zlistx_first (sensors);
    while (one_sensor) {
        if (!sensor_function) {
            zlistx_add_end (return_sensor_list, (void *) one_sensor);
        }
        else if (streq (sensor_function, bios_proto_ext_string (one_sensor, "sensor_function", ""))) {
            zlistx_add_end (return_sensor_list, (void *) one_sensor);
        }
        one_sensor = (bios_proto_t *) zlistx_next (sensors);
    }
    return return_sensor_list;
}

//  --------------------------------------------------------------------------
//  Checks if sensors attributes are ok.
//  Returns 'true' if we have all necessary information
//  Returns 'false' if some must-have information is missing
//  Writes to log detailed information "what is missing"

static bool
s_is_sensor_correct (bios_proto_t *sensor)
{
    assert (sensor);
    const char *logical_asset = bios_proto_ext_string (sensor, "logical_asset", NULL);
    const char *port = bios_proto_ext_string (sensor, "port", NULL);
    const char *parent_name = bios_proto_aux_string (sensor, "parent_name.1", NULL);

    if (!logical_asset) {
        log_error (
                "Attribute '%s' is missing from '%s' field of message where asset name = '%s'. "
                "This message is not stored.",
                "logical_asset", "ext", bios_proto_name (sensor));
        return false;
    }
    if (!parent_name) {
        log_error (
                "Attribute '%s' is missing from '%s' field of message where asset name = '%s'. "
                "This message is not stored.",
                "parent_name.1", "ext", bios_proto_name (sensor));
        return false;
    }
    if (!port) {
        log_error (
                "Attribute '%s' is missing from '%s' field of message where asset name = '%s'. "
                "This message is not stored.",
                "port", "ext", bios_proto_name (sensor));
        return false;
    }
    return true;
}

//  --------------------------------------------------------------------------
//  Store asset, takes ownership of the message

void
data_asset_store (data_t *self, bios_proto_t **message_p)
{
    assert (self);
    assert (message_p);
    if (!*message_p)
        return;
    bios_proto_t *message = *message_p;

    const char *operation = bios_proto_operation (message);
    const char *type = bios_proto_aux_string (message, "type", "");
    const char *subtype = bios_proto_aux_string (message, "subtype", "");

    if (  (streq (type, "device")) &&
         !(streq (subtype, "sensor") )
       )
    {
        // We are not interested in the 'device's that are not 'sensor's!
        bios_proto_destroy (message_p);
        *message_p = NULL;
        return;
    }

    if (streq (operation, BIOS_PROTO_ASSET_OP_CREATE) ) {
        if ( !streq (type, "device") ) {
            // So, if NOT "device" is created -> always do reconfiguration
            self->is_reconfig_needed = true;
            zhashx_update (self->all_assets, bios_proto_name (message), (void *) message);
            *message_p = NULL;
            return;
        }
        // So, we have "device" and it is "sensor"!
        // lets check, that sensor has all necessary information
        if ( s_is_sensor_correct (message) ) {
            // So, it is ok -> store it
            self->is_reconfig_needed = true;
            zhashx_update (self->all_assets, bios_proto_name (message), (void *) message);
            *message_p = NULL;
            return;
        } else {
            // no log message is here, as "s_is_sensor_correct" already wrote all detailed information
            bios_proto_destroy (message_p);
            *message_p = NULL;
            return;
        }
    } else
    if ( streq (operation, BIOS_PROTO_ASSET_OP_UPDATE) ) {
        if ( !streq (type, "device") ) {
            // So, if NOT "device" is UPDATED -> do reconfiguration only if TOPOLGY had changed
            // Look for the asset
            bios_proto_t *asset = (bios_proto_t*) zhashx_lookup (self->all_assets, bios_proto_name (message));
            if ( asset == NULL ) { // if the asset was not known (for any reason)
                self->is_reconfig_needed = true;
            }
            else {
                // BIOS-2484: start update of the asset should trigger reconfiguration if topology changed
                // if asset is known we need to check, if physical topology had changed
                // Actually, the chain is: dc-room-row-rack-device-device -> max 5 level parents can be
                // But here, we start from rack -> only 3 level is available at maximum
                if ( streq (bios_proto_aux_string (asset, "parent_name.1", ""),
                            bios_proto_aux_string (message, "parent_name.1", "")) ||
                     streq (bios_proto_aux_string (asset, "parent_name.2", ""),
                            bios_proto_aux_string (message, "parent_name.2", "")) ||
                     streq (bios_proto_aux_string (asset, "parent_name.3", ""),
                            bios_proto_aux_string (message, "parent_name.3", ""))
                   )
                {
                    self->is_reconfig_needed = true;
                }
                // BIOS-2484: end
            }
            zhashx_update (self->all_assets, bios_proto_name (message), (void *) message);
            *message_p = NULL;
            return;
        }
        // So, we have "device" and it is "sensor"!
        // lets check, that sensor has all necessary information
        if ( s_is_sensor_correct (message) ) {
            // So, it is ok -> store it
            self->is_reconfig_needed = true;
            zhashx_update (self->all_assets, bios_proto_name (message), (void *) message);
            *message_p = NULL;
            return;
        } else {
            // no log message is here, as "s_is_sensor_correct" already wrote all detailed information
            bios_proto_destroy (message_p);
            *message_p = NULL;
            return;
        }
    } else
    if (streq (operation, BIOS_PROTO_ASSET_OP_DELETE) ||
        streq (operation, BIOS_PROTO_ASSET_OP_RETIRE))
    {
        void *exists = zhashx_lookup (self->all_assets, bios_proto_name (message));
        if (exists)
            self->is_reconfig_needed = true;
        zhashx_delete (self->all_assets, bios_proto_name (message));
        bios_proto_destroy (message_p);
        *message_p = NULL;
        return;
    }
    else {
        log_debug ("Msg: op='%s', asset_name='%s' is not interesting", operation, bios_proto_name (message));
        bios_proto_destroy (message_p);
        *message_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Update list of metrics produced by composite_metrics

void
data_set_produced_metrics (data_t *self,const std::set <std::string> &metrics)
{
    assert (self);
    self->produced_metrics.clear();
    self->produced_metrics = metrics;
}

//  --------------------------------------------------------------------------
//  Get list of metrics produced by composite_metrics

std::set <std::string>
data_get_produced_metrics (data_t *self)
{
    assert (self);
    return self->produced_metrics;
}

//  --------------------------------------------------------------------------
//  Returns 'true' if some of recently added asset requires the reconfiguration
//                 or if reconfiguration was done in 'inconsistent' state
//                 and we MUST reconfigure one more time

bool
data_is_reconfig_needed (data_t *self)
{
    assert (self);
    return self->is_reconfig_needed;
}

//  --------------------------------------------------------------------------
//  Get asset names
//  The caller is responsible for destroying the return value when finished with it.

zlistx_t *
data_asset_names (data_t *self)
{
    assert (self);
    zlistx_t *list = zhashx_keys (self->all_assets);
    zlistx_set_comparator (list, (czmq_comparator *) strcmp);
    return list;
}

//  --------------------------------------------------------------------------
//  Get information for any given asset name if it is known or NULL otherwise
//  Ownership is NOT transferred

bios_proto_t *
data_asset (data_t *self, const char *name)
{
    assert (self);
    assert (name);
    return (bios_proto_t *) zhashx_lookup (self->all_assets, name);
}

//  --------------------------------------------------------------------------
//  Save data to disk
//  0 - success, -1 - error

int
data_save (data_t *self, const char * filename)
{
    assert (self);
    zconfig_t *root = zconfig_new ("nobody_cares", NULL);
    zconfig_t *assets = zconfig_new ("assets", root);
    int i = 1;
    for (bios_proto_t *bmsg = (bios_proto_t*) zhashx_first (self->all_assets);
                       bmsg != NULL;
                       bmsg = (bios_proto_t*) zhashx_next (self->all_assets))
    {
        zconfig_t *item = zconfig_new (std::to_string (i).c_str(), assets);
        zconfig_put (item, "name", bios_proto_name (bmsg));
        zconfig_put (item, "operation", bios_proto_operation (bmsg));

        zhash_t *aux = bios_proto_aux (bmsg);
        if ( aux ) {
            for (const char *aux_value = (const char*) zhash_first (aux);
                    aux_value != NULL;
                    aux_value = (const char*) zhash_next (aux))
            {
                const char *aux_key = (const char*) zhash_cursor (aux);
                char *item_key;
                int r = asprintf (&item_key, "aux.%s", aux_key);
                assert (r != -1);   // make gcc @ rhel happy
                zconfig_put (item, item_key, aux_value);
                zstr_free (&item_key);
            }
        }
        zhash_t *ext = bios_proto_ext (bmsg);
        if ( ext ) {
            for (const char *value = (const char*) zhash_first (ext);
                    value != NULL;
                    value = (const char*) zhash_next (ext))
            {
                const char *key = (const char*) zhash_cursor (ext);
                char *item_key;
                int r = asprintf (&item_key, "ext.%s", key);
                assert (r != -1);   // make gcc @ rhel happy
                zconfig_put (item, item_key, value);
                zstr_free (&item_key);
            }
        }
    }
    zconfig_t *metrics = zconfig_new ("produced_metrics", root);
    i = 1;
    for ( const auto &metric_topic : self->produced_metrics ) {
        zconfig_put (metrics, std::to_string (i).c_str(), metric_topic.c_str() );
        i++;
    }
    int r = zconfig_save (root, filename);
    zconfig_destroy (&root);
    return r;
}

//  --------------------------------------------------------------------------
//  Load data from disk
//  0 - success, -1 - error

data_t *
data_load (const char *filename)
{
    if ( !filename )
        return NULL;

    zconfig_t *root = zconfig_load (filename);
    if (!root)
        return NULL;

    data_t *self = data_new ();
    if (!self) {
        zconfig_destroy (&root);
        return NULL;
    }
    zconfig_t *sub_config = zconfig_child (root); // first child
    for ( ; sub_config != NULL; sub_config = zconfig_next (sub_config) ) { // next child
        const char *sub_key = zconfig_name (sub_config);
        if ( strncmp (sub_key, "assets", 6) == 0 ) {
            zconfig_t *key_config = zconfig_child (sub_config); // actually represents one asset
            for ( ; key_config != NULL; key_config = zconfig_next (key_config))
            {
                // 1. create bmsg
                bios_proto_t *bmsg = bios_proto_new (BIOS_PROTO_ASSET);
                bios_proto_set_name (bmsg, zconfig_get (key_config, "name", ""));
                bios_proto_set_operation (bmsg, zconfig_get (key_config, "operation", ""));

                // 2. put aux things
                zconfig_t *bmsg_config = zconfig_child (key_config);
                for (; bmsg_config != NULL; bmsg_config = zconfig_next (bmsg_config))
                {
                    const char *bmsg_key = zconfig_name (bmsg_config);
                    if (strncmp (bmsg_key, "aux.", 4) == 0)
                        bios_proto_aux_insert (bmsg, (bmsg_key+4), zconfig_value (bmsg_config));
                    if (strncmp (bmsg_key, "ext.", 4) == 0)
                        bios_proto_ext_insert (bmsg, (bmsg_key+4), zconfig_value (bmsg_config));
                }
                zhashx_update (self->all_assets, zconfig_get (key_config, "name", ""), bmsg);
            }
            continue;
        }
        if ( strncmp (sub_key, "produced_metrics", 16) ) {
            zconfig_t *key_config = zconfig_child (sub_config); // actually represents one metric
            for ( ; key_config != NULL; key_config = zconfig_next (key_config))
            {
                self->produced_metrics.insert (zconfig_value (key_config));
            }
            continue;
        }
        // if we are here, then unexpected config subtree found
        log_info ("key '%s' is not supported", sub_key);
    }
    zconfig_destroy (&root);
    return self;
}

//  --------------------------------------------------------------------------
//  Destroy the data

void
data_destroy (data_t **self_p)
{
    if (!self_p)
        return;
    if (*self_p) {
        data_t *self = *self_p;
        //  Free class properties here
        zhashx_destroy (&self->all_assets);
        zhashx_destroy (&self->last_configuration);
        self->produced_metrics.clear();
        //  Free object itself
        free (self);
        *self_p = NULL;
    }
}

//  --------------------------------------------------------------------------
//  Self test of this class

//  Helper test function
//  create new ASSET message of type bios_proto_t
static bios_proto_t *
test_asset_new (const char *name, const char *operation)
{
    assert (name);
    assert (operation);

    bios_proto_t *asset = bios_proto_new (BIOS_PROTO_ASSET);
    bios_proto_set_name (asset, "%s", name);
    bios_proto_set_operation (asset, "%s", operation);
    return asset;
}

//  Helper test function
//  0 - same, 1 - different
static int
test_zlistx_compare (zlistx_t *expected, zlistx_t **received_p, bool verbose = false)
{
    assert (expected);
    assert (received_p && *received_p);
    zlistx_t *received = *received_p;

    const char *cursor = (const char *) zlistx_first (expected);
    while (cursor) {
        void *handle = zlistx_find (received, (void *) cursor);
        if (!handle) {
            if ( verbose )
                log_debug ("expected but not found: %s", cursor);
            zlistx_destroy (received_p);
            *received_p = NULL;
            return 1;
        }
        zlistx_delete (received, handle);
        cursor = (const char *) zlistx_next (expected);
    }
    int rv = 1;
    if (zlistx_size (received) == 0) {
        rv = 0;
    } else {
        if ( verbose ) {
            const char *element = (const char *) zlistx_first (received);
            log_debug ("received but not expected:");
            while (element) {
                log_debug ("\t%s", element);
                element = (const char *) zlistx_next (received);
            }
        }
    }
    zlistx_destroy (received_p);
    *received_p = NULL;
    return rv;
}

void
data_test (bool verbose)
{
    if ( verbose )
        log_set_level (LOG_DEBUG);

    printf (" * data: \n");
    //  @selftest
    //  =================================================================
    if ( verbose )
        log_debug ("Test1: Simple create/destroy test");
    data_t *self = data_new ();
    assert (self);

    data_destroy (&self);
    assert (self == NULL);

    data_destroy (&self);
    assert (self == NULL);

    self = data_new ();

    //  =================================================================
    if ( verbose )
        log_debug ("Test2: data_asset_store()/data_reassign_needed()/data_is_reconfig_needed() for CREATE operation");
    // asset
    zlistx_t *assets_expected = zlistx_new ();
    zlistx_set_destructor (assets_expected, (czmq_destructor *) zstr_free);
    zlistx_set_duplicator (assets_expected, (czmq_duplicator *) strdup);
    zlistx_set_comparator (assets_expected, (czmq_comparator *) strcmp);

    bios_proto_t *asset = NULL;

    if ( verbose )
        log_debug ("\tCREATE 'DC-Rozskoky' as datacenter");
    asset = test_asset_new ("DC-Rozskoky", BIOS_PROTO_ASSET_OP_CREATE); // 1
    bios_proto_aux_insert (asset, "parent", "%s", "0");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "datacenter");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    bios_proto_ext_insert (asset, "max_power" , "%s",  "2");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "DC-Rozskoky");

    if ( verbose )
        log_debug ("\tCREATE 'Lazer game' as room");
    asset = test_asset_new ("Lazer game", BIOS_PROTO_ASSET_OP_CREATE); // 2
    bios_proto_aux_insert (asset, "parent", "%s", "1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "room");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Lazer game");

    if ( verbose )
        log_debug ("\tCREATE 'Curie' as room");
    asset = test_asset_new ("Curie", BIOS_PROTO_ASSET_OP_CREATE); // 3
    bios_proto_aux_insert (asset, "parent", "%s", "1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "room");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Curie");

    if ( verbose )
        log_debug ("\tCREATE 'Lazer game.Row01' as row");
    asset = test_asset_new ("Lazer game.Row01", BIOS_PROTO_ASSET_OP_CREATE); // 4
    bios_proto_aux_insert (asset, "parent", "%s", "2");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "row");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Lazer game.Row01");

    if ( verbose ) {
        log_debug ("\tCREATE 'Sensor01' as sensor");
        log_debug ("\t\tSituation: sensor asset message arrives before asset specified in logical_asset");
    }
    asset = test_asset_new ("Sensor01", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH1");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "10");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == true);
    zlistx_add_end (assets_expected, (void *) "Sensor01");
    data_save (self, "state_file");
    data_t *ndata = data_load ("state_file");
    assert (ndata);
    data_save (ndata, "state_file2");
    data_destroy (&ndata);

    {
        zlistx_t *received = data_asset_names (self);
        assert (received);

        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);
    }

    if ( verbose )
        log_debug ("\tCREATE 'Rack01' as rack");
    asset = test_asset_new ("Rack01", BIOS_PROTO_ASSET_OP_CREATE); // 5
    bios_proto_aux_insert (asset, "parent", "%s", "4");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "rack");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    bios_proto_ext_insert (asset, "u_size" , "%s",  "42");
    bios_proto_ext_insert (asset, "description" , "%s",  "Lorem ipsum asd asd asd asd asd asd asd");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Rack01");

    if ( verbose )
        log_debug ("\tCREATE 'Rack02' as rack");
    asset = test_asset_new ("Rack02", BIOS_PROTO_ASSET_OP_CREATE); // 6
    bios_proto_aux_insert (asset, "parent", "%s", "4");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "rack");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    bios_proto_ext_insert (asset, "u_size" , "%s",  "42");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Rack02");

    if ( verbose )
        log_debug ("\tCREATE 'Curie.Row01' as row");
    asset = test_asset_new ("Curie.Row01", BIOS_PROTO_ASSET_OP_CREATE); // 7
    bios_proto_aux_insert (asset, "parent", "%s", "3");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "row");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Curie.Row01");

    if ( verbose )
        log_debug ("\tCREATE 'Curie.Row02' as row");
    asset = test_asset_new ("Curie.Row02", BIOS_PROTO_ASSET_OP_CREATE); // 8
    bios_proto_aux_insert (asset, "parent", "%s", "3");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "row");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Curie.Row02");

    if ( verbose )
        log_debug ("\tCREATE 'Rack03' as rack");
    asset = test_asset_new ("Rack03", BIOS_PROTO_ASSET_OP_CREATE); // 9
    bios_proto_aux_insert (asset, "parent", "%s", "7");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "rack");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    bios_proto_ext_insert (asset, "u_size" , "%s",  "42");
    bios_proto_ext_insert (asset, "description" , "%s",  "Lorem ipsum asd asd asd asd asd asd asd");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Rack03");

    if ( verbose )
        log_debug ("\tCREATE 'Rack04' as rack");
    asset = test_asset_new ("Rack04", BIOS_PROTO_ASSET_OP_CREATE); // 10
    bios_proto_aux_insert (asset, "parent", "%s", "8");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "rack");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    bios_proto_ext_insert (asset, "u_size" , "%s",  "42");
    bios_proto_ext_insert (asset, "description" , "%s",  "Lorem ipsum asd asd asd asd asd asd asd");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Rack04");

    if ( verbose )
        log_debug ("\tCREATE 'Rack01.ups1' as ups");
    asset = test_asset_new ("Rack01.ups1", BIOS_PROTO_ASSET_OP_CREATE); // 11
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "ups");
    bios_proto_aux_insert (asset, "parent", "%s", "5");
    bios_proto_ext_insert (asset, "abc.d", "%s", " ups string 1");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tCREATE 'Sensor02' as sensor");
    asset = test_asset_new ("Sensor02", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH2");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "20");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor02");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor03' as sensor");
    asset = test_asset_new ("Sensor03", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH3");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor03");

    if ( verbose ) {
        log_debug ("\tCREATE 'Sensor04' as sensor");
        log_debug ("\t\tImportant info is missing");
    }
    asset = test_asset_new ("Sensor04", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH3");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    // logical_asset missing
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose ) {
        log_debug ("\tCREATE 'Sensor06' as sensor");
        log_debug ("\t\tImportant info is missing");
    }
    asset = test_asset_new ("Sensor06", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    // parent_name.1 missing
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH3");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose ) {
        log_debug ("\tCREATE 'Sensor07' as sensor");
        log_debug ("\t\tImportant info is missing");
    }
    asset = test_asset_new ("Sensor07", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    // port missing
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "3");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "30");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tCREATE 'Sensor08' as sensor");
    asset = test_asset_new ("Sensor08", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH4");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor08");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor09' as sensor");
    asset = test_asset_new ("Sensor09", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH5");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.0");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "2.0");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "top");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor09");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor10' as sensor");
    asset = test_asset_new ("Sensor10", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH6");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "top");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor10");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor11' as sensor");
    asset = test_asset_new ("Sensor11", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH7");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "15.5");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "20.7");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "top");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor11");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor12' as sensor");
    asset = test_asset_new ("Sensor12", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH8");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "0");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "0");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "neuvedeno");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor12");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor13' as sensor");
    asset = test_asset_new ("Sensor13", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH9");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "-1");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "top");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor13");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor14' as sensor");
    asset = test_asset_new ("Sensor14", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH10");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Curie");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor14");

    if ( verbose )
        log_debug ("\tCREATE 'Sensor15' as sensor");
    asset = test_asset_new ("Sensor15", BIOS_PROTO_ASSET_OP_CREATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH11");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "1.4");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-1");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor15");

    printf ("TRACE ---===### (Test block -1-) ###===---\n");
    {
        zlistx_t *received = data_asset_names (self);
        assert (received);

        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);

        asset = data_asset (self, "DC-Rozskoky");
        assert (asset);
        assert (streq (bios_proto_aux_string (asset, "type", ""), "datacenter"));
        assert (streq (bios_proto_name (asset), "DC-Rozskoky"));

        asset = data_asset (self, "Lazer game");
        assert (asset);
        assert (streq (bios_proto_aux_string (asset, "type", ""), "room"));
        assert (streq (bios_proto_name (asset), "Lazer game"));
        assert (streq (bios_proto_aux_string (asset, "parent", ""), "1"));

        asset = data_asset (self, "Curie");
        assert (asset);
        assert (streq (bios_proto_aux_string (asset, "type", ""), "room"));
        assert (streq (bios_proto_name (asset), "Curie"));
        assert (streq (bios_proto_aux_string (asset, "parent", ""), "1"));

        asset = data_asset (self, "Lazer game.Row01");
        assert (asset);
        assert (streq (bios_proto_aux_string (asset, "type", ""), "row"));
        assert (streq (bios_proto_name (asset), "Lazer game.Row01"));
        assert (streq (bios_proto_aux_string (asset, "parent", ""), "2"));

        asset = data_asset (self, "Rack01");
        assert (asset);
        asset = data_asset (self, "Rack02");
        assert (asset);
        asset = data_asset (self, "Rack01.ups1");
        assert (asset == NULL);
        asset = data_asset (self, "non-existing-sensor");
        assert (asset == NULL);
/*
        zlistx_t *sensors = data_sensor (self, "Non-existing-dc", NULL);
        assert (sensors == NULL);

        sensors = data_sensor (self, "DC-Rozskoky", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie", NULL);
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor14"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH10"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), ""));
            item = (bios_proto_t *) zlistx_next (sensors);

        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie", "input");
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game.Row01", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row01", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row02", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor13"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH9"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "-1"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "1"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor15"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH11"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "1.4"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-1"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row02", "input");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor13"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row02", "output");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor15"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack02", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor08"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH4"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "1"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "1"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor09"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH5"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "2.0"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "2.0"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack02", "input");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor08"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack02", "output");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor09"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack03", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack04", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack01", NULL);
        assert (zlistx_size (sensors) == 6);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor01"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH1"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "1"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "10"));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor02"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH2"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "2"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "20"));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor03"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH3"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "3"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "30"));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor10"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH6"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), ""));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor11"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH7"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "15.5"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "20.7"));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor12"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH8"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "neuvedeno"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "0"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "0"));

        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack01", "input");
        assert (zlistx_size (sensors) == 3);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor01"));
            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor02"));
            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor03"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack01", "output");
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor10"));
            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor11"));
        }
        zlistx_destroy (&sensors);
*/
    }

    if ( verbose )
        log_debug ("\tCREATE 'ups2' as ups");
    asset = test_asset_new ("ups2", BIOS_PROTO_ASSET_OP_CREATE); // 12
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "ups");
    bios_proto_aux_insert (asset, "parent", "%s", "10");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == false);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor01'");
    asset = test_asset_new ("Sensor01", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH1");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "-5.2");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "bottom");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor02'");
    asset = test_asset_new ("Sensor02", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "12");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "ups2");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH1");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "-7");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-4.14");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor03'");
    asset = test_asset_new ("Sensor03", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH3");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor10'");
    asset = test_asset_new ("Sensor10", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH2");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-0.16");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "top");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Rack01");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tRETIRE 'Sensor11'");
    asset = test_asset_new ("Sensor11", BIOS_PROTO_ASSET_OP_RETIRE);
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    void *handle = zlistx_find (assets_expected, (void *) "Sensor11");
    zlistx_delete (assets_expected, handle);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor08'");
    asset = test_asset_new ("Sensor08", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH4");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "2.0");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "12");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "middle");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "input");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "DC-Rozskoky");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor09'");
    asset = test_asset_new ("Sensor09", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH5");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "5");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "50");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor04'");
    asset = test_asset_new ("Sensor04", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH12");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "1");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "1");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Lazer game");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor04");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor05'");
    asset = test_asset_new ("Sensor05", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH13");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "4");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-6");
    bios_proto_ext_insert (asset, "vertical_position", "%s", "top");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "DC-Rozskoky");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor05");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor06'");
    asset = test_asset_new ("Sensor06", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH14");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "-1.2");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-1.4");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "output");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Lazer game");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor06");

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor07'");
    asset = test_asset_new ("Sensor07", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH15");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "4");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-25");
    bios_proto_ext_insert (asset, "sensor_function", "%s", "ambient");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Curie.Row02");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    zlistx_add_end (assets_expected, (void *) "Sensor07");

    if ( verbose )
        log_debug ("\tDELETE 'Sensor12'");
    asset = test_asset_new ("Sensor12", BIOS_PROTO_ASSET_OP_DELETE);
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == false);
    handle = zlistx_find (assets_expected, (void *) "Sensor12");
    zlistx_delete (assets_expected, handle);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor14'");
    asset = test_asset_new ("Sensor14", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "12");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "ups2");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH10");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "This-asset-does-not-exist");
    data_asset_store (self, &asset);
    assert (data_is_reconfig_needed (self) == true);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tUPDATE 'Sensor15'");
    asset = test_asset_new ("Sensor15", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "11");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01.ups1");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH11");
    bios_proto_ext_insert (asset, "calibration_offset_t", "%s", "2");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-3");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "Curie");
    data_asset_store (self, &asset);
    data_reassign_sensors(self);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tDELETE 'Sensor13'");
    asset = test_asset_new ("Sensor13", BIOS_PROTO_ASSET_OP_DELETE);
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    data_reassign_sensors(self);
    handle = zlistx_find (assets_expected, (void *) "Sensor13");
    zlistx_delete (assets_expected, handle);

    printf ("TRACE ---===### (Test block -2-) ###===---\n");
    {
        zlistx_t *received = data_asset_names (self);
        assert (received);
        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);

        asset = data_asset (self, "ups2");
        assert (asset == NULL);

        asset = data_asset (self, "This-asset-does-not-exist");
        assert (asset == NULL);
/*
        zlistx_t *sensors = data_sensor (self, "This-asset-does-not-exist", NULL);
        assert (sensors == NULL);

        sensors = data_sensor (self, "DC-Rozskoky", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor08"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH4"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "2.0"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "12"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor05"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH13"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "4"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-6"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "DC-Rozskoky", "input");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor08"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "DC-Rozskoky", "output");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor05"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor04"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH12"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "1"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "1"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor06"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH14"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "-1.2"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-1.4"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game", "input");
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game", "output");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor06"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game", "");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor04"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie", NULL);
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor15"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH11"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "2"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-3"));
            item = (bios_proto_t *) zlistx_next (sensors);

        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie", "input");
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Lazer game.Row01", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row01", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row02", NULL);
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor09"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH5"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "5"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "50"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor07"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH15"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "ambient"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "4"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-25"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row02", "input");
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Curie.Row02", "ambient");
        assert (zlistx_size (sensors) == 1);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor07"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack02", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack03", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack04", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack01", NULL);
        assert (zlistx_size (sensors) == 4);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor01"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH1"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "bottom"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "-5.2"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), ""));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor02"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "ups2"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH1"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "-7"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-4.14"));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor03"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH3"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), ""));

            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor10"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH2"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-0.16"));
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack01", "output");
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor03"));
            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor10"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        sensors = data_sensor (self, "Rack01", "input");
        assert (zlistx_size (sensors) == 2);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor01"));
            item = (bios_proto_t *) zlistx_next (sensors);
            assert (streq (bios_proto_name (item), "Sensor02"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);
        */
    }

    if ( verbose )
        log_debug ("\tDELETE 'Sensor15'");
    asset = test_asset_new ("Sensor15", BIOS_PROTO_ASSET_OP_DELETE);
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    data_asset_store (self, &asset);
    data_reassign_sensors(self);
    handle = zlistx_find (assets_expected, (void *) "Sensor15");
    zlistx_delete (assets_expected, handle);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tDELETE 'Curie.Row02'");
    asset = test_asset_new ("Curie.Row02", BIOS_PROTO_ASSET_OP_DELETE);
    bios_proto_aux_insert (asset, "type", "%s", "row");
    bios_proto_aux_insert (asset, "subtype", "%s", "unknown");
    data_asset_store (self, &asset);
    data_reassign_sensors(self);

    void *to_delete = zlistx_find  (assets_expected, (void *) "Curie.Row02");
    assert (to_delete);
    int rv = zlistx_delete (assets_expected, to_delete);
    assert (rv == 0);
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tCREATE 'Sensor16' as sensor");
    asset = test_asset_new ("Sensor16", BIOS_PROTO_ASSET_OP_UPDATE);
    bios_proto_aux_insert (asset, "parent", "%s", "13");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "nas rack controller");
    bios_proto_aux_insert (asset, "status", "%s", "active");
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "sensor");
    bios_proto_ext_insert (asset, "port", "%s", "TH2");
    bios_proto_ext_insert (asset, "calibration_offset_h", "%s", "-3.51");
    bios_proto_ext_insert (asset, "logical_asset", "%s", "DC-Rozskoky");
    data_asset_store (self, &asset);
    data_reassign_sensors(self);
    zlistx_add_end (assets_expected, (void *) "Sensor16");
    assert (data_is_reconfig_needed (self) == true);

    if ( verbose )
        log_debug ("\tCREATE 'nas rack constroller' as rack controller");
    asset = test_asset_new ("nas rack controller", BIOS_PROTO_ASSET_OP_CREATE); // 12
    bios_proto_aux_insert (asset, "type", "%s", "device");
    bios_proto_aux_insert (asset, "subtype", "%s", "rack controller");
    bios_proto_aux_insert (asset, "parent", "%s", "5");
    bios_proto_aux_insert (asset, "parent_name.1", "%s", "Rack01");
    data_asset_store (self, &asset);
    data_reassign_sensors(self);
    // "true" is expected, because data_reassign_sensors also changes the "is_reconfiguration_needed"
    // when detailed information about the asset is not known
    assert (data_is_reconfig_needed (self) == true);

    printf ("TRACE ---===### (Test block -3-) ###===---\n");
    {
        zlistx_t *received = data_asset_names (self);
        assert (received);
        int rv = test_zlistx_compare (assets_expected, &received, verbose);
        assert (rv == 0);
/*
        zlistx_t *sensors = data_sensor (self, "DC-Rozskoky", NULL);
        assert (zlistx_size (sensors) == 3);
        {
            bios_proto_t *item = (bios_proto_t *) zlistx_first (sensors);
            assert (streq (bios_proto_name (item), "Sensor08"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH4"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "middle"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "input"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "2.0"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "12"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor05"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "Rack01.ups1"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH13"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), "top"));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), "output"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), "4"));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-6"));
            item = (bios_proto_t *) zlistx_next (sensors);

            assert (streq (bios_proto_name (item), "Sensor16"));
            assert (streq (bios_proto_aux_string (item, "parent_name.1", ""), "nas rack controller"));
            assert (streq (bios_proto_ext_string (item, "port", ""), "TH2"));
            assert (streq (bios_proto_ext_string (item, "vertical_position", ""), ""));
            assert (streq (bios_proto_ext_string (item, "sensor_function", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_t", ""), ""));
            assert (streq (bios_proto_ext_string (item, "calibration_offset_h", ""), "-3.51"));
            item = (bios_proto_t *) zlistx_next (sensors);
        }
        zlistx_destroy (&sensors);

        asset = data_asset (self, "Curie");
        assert (asset);
        assert (streq (bios_proto_aux_string (asset, "type", ""), "room"));
        assert (streq (bios_proto_name (asset), "Curie"));
        assert (streq (bios_proto_aux_string (asset, "parent", ""), "1"));

        sensors = data_sensor (self, "Curie", NULL);
        assert (zlistx_size (sensors) == 0);
        zlistx_destroy (&sensors);

        asset = data_asset (self, "Curie.Row02");
        assert (asset == NULL);

        sensors = data_sensor (self, "Curie.Row02", NULL);
        assert (sensors == NULL);
        */
    }


    data_t *newdata = data_new();
    std::set <std::string> newset{"sdlkfj"};
    data_set_produced_metrics (newdata, newset);

    data_destroy (&newdata);

    zlistx_destroy (&assets_expected);
    data_destroy (&self);
    //  @end
    printf (" * data: OK\n");
}
