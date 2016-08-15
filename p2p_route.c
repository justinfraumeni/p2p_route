/* P2P_ROUTE - OLSRD2 plugin
 *  
 * Author: Justin Fraumeni
 * Date: 7/25/16
 * 
 * OONF OLSRD3 plugin to enable multigroup routing between WiFi-P2P groups.
 * When provided with a control interface name, p2p_route dynamically configures 
 * virtual group interfaces as they are created during a olsrd2 session.
 *
*/

#include <sys/socket.h>
#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/socket.h>

#include "config/cfg_cmd.h"
#include "subsystems/os_linux/os_system_linux.h"
#include "common/common_types.h"
#include "common/netaddr.h"
#include "config/cfg_schema.h"
#include "config/cfg_db.h"
#include "core/oonf_cfg.h"
#include "core/oonf_subsystem.h"
#include "subsystems/os_interface.h"
#include "subsystems/os_linux/os_interface_linux.h"
#include <regex.h>
#include "nhdp/nhdp_interfaces.h"
#include "p2p_route/p2p_route.h"
#include "core/oonf_libdata.h"
#include "core/oonf_logging.h"
#include "core/oonf_logging_cfg.h"
#define LOG_P2P_ROUTE _p2p_route_subsystem.logging

struct _p2p_route_config {
  
  // master p2p control interface
  char * control_interface;

};

/* prototypes */
static int _init(void);
static void _cleanup(void);

static void _cb_config_changed(void);
static int _cb_interface_changed(struct os_interface_listener *if_listener);
static void _cb_interface_listener_netlink(struct nlmsghdr *hdr);
static void _query_interface_links(void);
static void _p2p_add_group_interface(unsigned int if_index);
static void _p2p_remove_group_interface(unsigned int if_index);
static bool _p2p_interface_exists(unsigned int if_index);
static void _cb_timeout_netlink(void);
static void _cb_done_netlink(uint32_t seq);
static void _cb_error_netlink(uint32_t seq, int error);

static struct cfg_schema_entry _p2p_route_entries[] = {
  CFG_MAP_STRING(_p2p_route_config, control_interface, "control_interface", "all", "P2P master control interface to track.")
};

static struct cfg_schema_section _p2p_route_section = {
  .type = OONF_P2P_ROUTE_SUBSYSTEM,
  .cb_delta_handler = _cb_config_changed,
  .entries = _p2p_route_entries,
  .entry_count = ARRAYSIZE(_p2p_route_entries)
};

static struct _p2p_route_config _config;

//should be more dependencies than this, update and  
static const char * _dependencies[] = {
  OONF_OS_INTERFACE_SUBSYSTEM
};

//we care only about link status events at layer 2
static const uint32_t _netlink_mcast[] = {
  RTNLGRP_LINK
};

static struct oonf_subsystem _p2p_route_subsystem = {
  .name = OONF_P2P_ROUTE_SUBSYSTEM,
  .dependencies = _dependencies,
  .dependencies_count = ARRAYSIZE(_dependencies),
  .descr = "OONF plugin to facilitate p2p multigroup routing.",
  .author = "Justin Fraumeni",
  .cfg_section = &_p2p_route_section,
  .init = _init,
  .cleanup = _cleanup
};

DECLARE_OONF_PLUGIN(_p2p_route_subsystem);
 
static struct os_system_netlink _netlink_receiver =  {

  .name = "p2p_route interface receiver",
  .used_by = &_p2p_route_subsystem,
  .cb_message = _cb_interface_listener_netlink,
  .cb_error = _cb_error_netlink,
  .cb_done = _cb_done_netlink,
  .cb_timeout = _cb_timeout_netlink
};

static struct os_system_netlink _netlink_query= {
  .name = "p2p_route interface query",
  .used_by = &_p2p_route_subsystem,
  .cb_message = _cb_interface_listener_netlink,
  .cb_error = _cb_error_netlink,
  .cb_done = _cb_done_netlink,
  .cb_timeout = _cb_timeout_netlink
};

/* global interface listener */
static struct os_interface_listener _if_listener = {
  .name = OS_INTERFACE_ANY,
  .mesh = false,
  .if_changed = _cb_interface_changed,
};

static int _init(void) {

  /* activate interface listener */
  if(os_system_linux_netlink_add(&_netlink_receiver, NETLINK_ROUTE)) {
    return -1;
  }

  if(os_system_linux_netlink_add(&_netlink_query, NETLINK_ROUTE)) {
    os_system_linux_netlink_remove(&_netlink_receiver);
    return -1;
  }
  if(os_system_linux_netlink_add_mc(&_netlink_receiver, _netlink_mcast, ARRAYSIZE(_netlink_mcast))) {
    os_system_linux_netlink_remove(&_netlink_receiver);
    os_system_linux_netlink_remove(&_netlink_query);
    return -1;
  }  
  os_interface_add(&_if_listener);
  return 0;
}

static void _cleanup(void) {
  os_interface_remove(&_if_listener);
  os_system_linux_netlink_remove(&_netlink_receiver);
  os_system_linux_netlink_remove(&_netlink_query);
}

static void _cb_error_netlink(uint32_t seq, int error){
  OONF_WARN(LOG_P2P_ROUTE, "p2p_route netlink error %d %d", seq, error);
}

static void _cb_timeout_netlink(void){
  OONF_WARN(LOG_P2P_ROUTE, "p2p_route netlink timeout");
}

static void _cb_done_netlink(uint32_t seq){
  OONF_WARN(LOG_P2P_ROUTE, "p2p_route done %u", seq);
}

static void _query_interface_links(void){
  uint8_t buffer[UIO_MAXIOV];
  struct nlmsghdr *msg;
  struct ifinfomsg *ifi;
   
  msg = (void *)&buffer[0];
  memset(buffer, 0, sizeof(buffer));
  msg->nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
  msg->nlmsg_type = RTM_GETLINK;

  msg->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));

  ifi = NLMSG_DATA(msg);
  ifi->ifi_family = AF_NETLINK;

  OONF_DEBUG(LOG_P2P_ROUTE, "querying interface link data");
  os_system_linux_netlink_send(&_netlink_query, msg);
  //request interface status info, on start, and maybe periodic.
}

static void _cb_interface_listener_netlink(struct nlmsghdr *hdr){

  char ifname[IF_NAMESIZE];
  struct ifinfomsg *ifi;
  char interface_ex[100];
  regex_t group_regex;

  //use regex to match format of group interface
  strcpy(interface_ex, "p2p-");
  strcpy(interface_ex, _config.control_interface);
  strcpy(interface_ex,  "-[0-9][0-9]*");
  regcomp(&group_regex, interface_ex, 0);

  ifi = (struct ifinfomsg *) NLMSG_DATA(hdr);

  if_indextoname(ifi->ifi_index, ifname);
  
  if(hdr->nlmsg_type == RTM_NEWLINK && !regexec(&group_regex, ifname, 0, NULL, 0)){

    OONF_WARN(LOG_P2P_ROUTE, "A p2p interface! %s", ifname);
   
    //if interface is up and is not in configuration, add it
    if((ifi->ifi_flags & IFF_UP) == IFF_UP && !_p2p_interface_exists(ifi->ifi_index)){
    
      OONF_DEBUG(LOG_P2P_ROUTE, "Interface is up! %s", ifname);
      OONF_DEBUG(LOG_P2P_ROUTE, "Interface is added to tracking! %s", ifname);
      _p2p_add_group_interface(ifi->ifi_index);
     	
    }else if(!((ifi->ifi_flags & IFF_UP) == IFF_UP) && _p2p_interface_exists(ifi->ifi_index)){

      //if matches and list contains, remove it
      OONF_DEBUG(LOG_P2P_ROUTE, "Interface is down! %s", ifname);
      OONF_DEBUG(LOG_P2P_ROUTE, "Interface is removed from tracking! %s", ifname);
      _p2p_remove_group_interface(ifi->ifi_index);
    }
  } else {
    OONF_DEBUG(LOG_P2P_ROUTE, "Not a p2p interface: %s", ifname);
  }
}

static bool _p2p_interface_exists(unsigned int if_index){

  char if_name[IF_NAMESIZE];
  if_indextoname(if_index, if_name);
  
  OONF_DEBUG(LOG_P2P_ROUTE, "Checking active configuration for p2p interface: %s", if_name); 

  if(cfg_db_find_namedsection(oonf_cfg_get_rawdb(),"interface", if_name) != NULL){

    OONF_DEBUG(LOG_P2P_ROUTE, "Found p2p interface in configuration: %s", if_name); 
    return true;
  }
  OONF_DEBUG(LOG_P2P_ROUTE, "Did not find p2p interface in configuration: %s", if_name); 
  return false;
}



static void _p2p_add_group_interface(unsigned int if_index){

   char ifname[IF_NAMESIZE];
   char ifbuf[IF_NAMESIZE];
   struct autobuf log;
   char add_if[IF_NAMESIZE + 10];
   const char * name;
   abuf_init(&log);
  
   if_indextoname(if_index, ifname);
   OONF_WARN(LOG_P2P_ROUTE, "Adding group interface: %s", ifname);
   name = cfg_get_phy_if(ifbuf, ifname);
   sprintf(add_if, "interface[%s].", name);

   if(cfg_cmd_handle_set(oonf_cfg_get_instance(), oonf_cfg_get_rawdb(), add_if, &log)){
     OONF_WARN(LOG_P2P_ROUTE, "Failed to add group interface: %s ", ifname);
     return;
   }
   oonf_cfg_trigger_commit();
  
}

static void _p2p_remove_group_interface(unsigned int if_index){

   char ifname[IF_NAMESIZE];
   char ifbuf[IF_NAMESIZE];
   struct autobuf log;
   char remove_if[IF_NAMESIZE + 10];
   const char * name;
   abuf_init(&log);
  
   if_indextoname(if_index, ifname);
   OONF_WARN(LOG_P2P_ROUTE, "Removing group interface: %s", ifname);
   name = cfg_get_phy_if(ifbuf, ifname);
   sprintf(remove_if, "interface[%s].", name);

   if(cfg_cmd_handle_remove(oonf_cfg_get_instance(), oonf_cfg_get_rawdb(), remove_if, &log)){
     OONF_WARN(LOG_P2P_ROUTE, "Failed to remove group interface: %s ", ifname);
     return;
   }
   oonf_cfg_trigger_commit();
}

//on update, check whether it matches group interface regex, if so register it.
static int _cb_interface_changed(struct os_interface_listener *if_listener __attribute__((unused))) {
  
  OONF_DEBUG(LOG_P2P_ROUTE, "Interfaces changed, trigger query.");
  _query_interface_links();

  return 0;
}

static void _cb_config_changed(void) {


  if(cfg_schema_tobin(&_config, _p2p_route_section.post, _p2p_route_entries, ARRAYSIZE(_p2p_route_entries))) {
    OONF_WARN(LOG_P2P_ROUTE, "Could not convert " OONF_P2P_ROUTE_SUBSYSTEM " config to bin");
    return;
  }
  OONF_DEBUG(LOG_P2P_ROUTE, "Updated config %s", _config.control_interface);
}
