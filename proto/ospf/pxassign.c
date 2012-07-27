/*
 * BIRD -- OSPF
 *
 * (c) 2012 Benjamin Paterson <benjamin@paterson.fr>
 *
 * Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: prefix assignment
 *
 * This implementation is based off of:
 * http://tools.ietf.org/html/draft-arkko-homenet-prefix-assignment-01
 *
 *
 */

#include "ospf.h"
#include "lib/md5.h"
#include <stdlib.h>
#include <stdio.h>
#include "sysdep/unix/linksys.h"

#ifdef OSPFv3

static struct prefix_node* assignment_find(struct ospf_iface *ifa, struct prefix *usp);
static int in_use(struct prefix *px, list used);
static void next_prefix(struct prefix *pxa, struct prefix *pxb);
static void find_used(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, list *used, ip_addr *steal_addr, unsigned int *steal_len,
                      unsigned int *found_steal, ip_addr *split_addr, unsigned int *split_len, unsigned int *found_split, u8 *lowest_pa_priority,
                      struct prefix_node *self_r_px);
static void try_reuse(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, list *used,
                      unsigned int *pxchoose_success, unsigned int *change, unsigned int length, struct prefix_node *self_r_px);
static void try_assign_unused(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, list *used,
                              unsigned int *pxchoose_success, unsigned int *change, unsigned int length, struct prefix_node *self_r_px);
static void try_steal(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, ip_addr *steal_addr, unsigned int *steal_len,
                      unsigned int *found_steal, u8 *lowest_pa_priority, unsigned int *pxchoose_success, unsigned int *change,
                      struct prefix_node *self_r_px);
static void try_split(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, ip_addr *split_addr, unsigned int *split_len,
                      unsigned int *found_split, unsigned int *pxchoose_success, unsigned int *change, unsigned int length);

static int
configure_ifa_add_prefix(ip_addr addr, unsigned int len,
                         u32 rid,
                         u32 my_rid,
                         int pa_priority,
                         struct ospf_iface *ifa)
{
  struct prefix_node *pxn;
  char cmd[128];
  char ip6addr[40];

  /* Add the prefix to the interface */
  pxn = mb_alloc(ifa->pool, sizeof(struct prefix_node));
  pxn->px.addr = addr;
  pxn->px.len = len;
  pxn->rid = rid;
  pxn->my_rid = my_rid;
  pxn->pa_priority = pa_priority;
  pxn->valid = 1;
  add_tail(&ifa->asp_list, NODE pxn);

  /* And then configure it to the system */
  // FIXME need a better way to do this.
  // FIXME #2 BIRD seems to create a new ospf_iface struct when addresses change on an interface.
  // Maybe the interfaces' asp_list should be placed elsewhere than in the ospf_iface struct.
  // FIXME #3 This should probably be in a sysdep file.
  ip_ntop(addr, ip6addr);
  snprintf(cmd, sizeof(cmd), "ip -6 addr add %s%x:%x/%d dev %s",
           ip6addr,
           my_rid >> 16,
           my_rid & 0xFFFF,
           len, ifa->iface->name);
  return system(cmd);
}

static int
configure_ifa_del_prefix(struct prefix_node *pxn, struct ospf_iface *ifa)
{
  char cmd[128];
  char ip6addr[40];
  int rv;

  /* Remove the prefix from the system */
  // FIXME need a better way to do this.
  // FIXME #2 BIRD seems to create a new ospf_iface struct when addresses change on an interface.
  // Maybe the interfaces' asp_list should be placed elsewhere than in the ospf_iface struct.
  // FIXME #3 This should probably be in a sysdep file.
  ip_ntop(pxn->px.addr, ip6addr);
  snprintf(cmd, sizeof(cmd), "ip -6 addr del %s%x:%x/%d dev %s",
           ip6addr,
           pxn->my_rid >> 16,
           pxn->my_rid & 0xFFFF,
           pxn->px.len, ifa->iface->name);
  rv = system(cmd);

  /* And from the internal datastructure */
  rem_node(NODE pxn);
  mb_free(pxn);


  return rv;
}

/**
 * find_next_tlv - find next TLV of specified type in AC LSA
 * @lsa: A pointer to the beginning of the body
 * @offset: Offset to the beginning of the body to start search
 * (must point to the beginning of a TLV)
 * @size: Size of the body to search
 * @type: The type of TLV to search for
 *
 * Returns a pointer to the beginning of the next TLV of specified type,
 * or null if there are no more TLVs of that type.
 * If @type is set to NULL, returns the next TLV, whatever the type.
 * Updates @offset to point to the next TLV, or to after the last TLV if
 * there are no more TLVs of the specified type.
 */
void *
find_next_tlv(void *lsa, int *offset, unsigned int size, u8 type)
{
  unsigned int bound = size - 4;
  int old_offset;

  u8 *tlv = (u8 *) lsa;
  while(*offset <= bound)
  {
    old_offset = *offset;
    *offset += LSA_AC_TLV_SPACE(((struct ospf_lsa_ac_tlv *)(tlv + *offset))->length);
    if(!type || ((struct ospf_lsa_ac_tlv *)(tlv + old_offset))->type == type)
      return tlv + old_offset;
  }

  return NULL;
}

/**
 * is_highest_rid - Determine if we have the highest RID on a link
 * among neighbors in state greater than @state and with same
 * prefix assignment priority
 * @ifa: The interface on which to perform the check
 */
/*static int
is_highest_rid(struct ospf_iface *ifa, u8 state)
{
  struct ospf_neighbor *n;

  WALK_LIST(n, ifa->neigh_list)
  {
    if(n->state >= state && n->rid > ifa->oa->po->router_id)
      return 0;
  }
  return 1;
}*/

/**
 * assignment_find - Check if we have already assigned a prefix
 * on this interface from a specified usable prefix, and return a pointer
 * to this assignment in the asp_list if it exists.
 *
 * @ifa: The current ospf_iface
 * @usp: The usable prefix
 */
static struct prefix_node *
assignment_find(struct ospf_iface *ifa, struct prefix *usp)
{
  //struct ospf_iface *ifa = usp->ifa;
  struct prefix_node *aspn;
  struct proto_ospf *po = ifa->oa->po;

  WALK_LIST(aspn, ifa->asp_list)
  {
    if(aspn->rid == po->router_id && net_in_net(aspn->px.addr, aspn->px.len, usp->addr, usp->len))
    {
      return aspn;
    }
  }
  return NULL;
}

/**
 * random_prefix - Select a pseudorandom sub-prefix of specified length
 * @px: A pointer to the prefix
 * @pxsub: A pointer to the sub-prefix. Length field must be set.
 * @i: Number of the iteration-
 */
static void
random_prefix(struct prefix *px, struct prefix *pxsub, u32 rid, struct ospf_iface *ifa, int i)
{
  struct MD5Context ctxt;
  char md5sum[16];

  MD5Init(&ctxt);
  MD5Update(&ctxt, ifa->iface->name, strlen(ifa->iface->name));
  MD5Update(&ctxt, (char *)&rid, sizeof(rid));
  MD5Update(&ctxt, (char *)&i, sizeof(i));
  MD5Final(md5sum, &ctxt);

  memcpy(&pxsub->addr, md5sum, 16);

  // clean up right part of prefix
  pxsub->addr = ipa_and(pxsub->addr, ipa_mkmask(pxsub->len));

  // clean up left part of prefix
  pxsub->addr = ipa_and(pxsub->addr, ipa_not(ipa_mkmask(px->len)));

  // set left part of prefix
  pxsub->addr = ipa_or(pxsub->addr, px->addr);
}

/**
 * in_use - Determine if a prefix is already in use
 * @px: The prefix of interest
 * @used: A list of struct prefix_node
 *
 * This function returns 1 if @px is a sub-prefix or super-prefix
 * of any of the prefixes in @used, 0 otherwise.
 */
static int
in_use(struct prefix *px, list used)
{
  struct prefix_node *pxn;

  WALK_LIST(pxn, used){
    if(net_in_net(px->addr, px->len, pxn->px.addr, pxn->px.len)
       || net_in_net(pxn->px.addr, pxn->px.len, px->addr, px->len))
      return 1;
  }
  return 0;
}

/**
 * next_prefix - Increment prefix to next non-covered/non-covering prefix
 * @pxa: The prefix to increment
 * @pxb: The covering/covered prefix
 *
 * This function calculates the next prefix of length
 * @pxa->len that is not covered by @pxb, and stores it in
 * @pxa. If there is no such prefix, stores IPA_NONE in @pxa->addr.
 */
static void
next_prefix(struct prefix *pxa, struct prefix *pxb)
{
  // if pxa is covering prefix
  if(pxa->len < pxb->len)
  {
    unsigned int i = (pxa->len - 1) / 32;
    u64 add; //hack, need a better way to detect overflow

    add = ((u64) pxa->addr.addr[i]) + (0x80000000 >> ((pxa->len -1) % 32));

    if(add < 0xFFFFFFFF)
    {
      pxa->addr.addr[i]  = (u32) add;
      return;
    }

    pxa->addr.addr[i--] = 0x00000000;
    while(i >= 0)
    {
      add = ((u64) pxa->addr.addr[i]) + 0x00000001;
      if(add < 0xFFFFFFFF)
      {
        pxa->addr.addr[i]  = (u32) add;
        return;
      }
      pxa->addr.addr[i--] = 0x00000000;
    }

    pxa->addr = IPA_NONE;
  }

  // otherwise, pxb is covering prefix
  unsigned int i = (pxb->len - 1) / 32;
  u64 add; //hack, need a better way to detect overflow

  add = ((u64) pxb->addr.addr[i]) + (0x80000000 >> ((pxb->len -1) % 32));
  if(add < 0xFFFFFFFF)
  {
    pxa->addr.addr[i]  = (u32) add;
    pxa->addr = ipa_and(pxa->addr, ipa_mkmask(pxb->len));
    return;
  }

  pxa->addr.addr[i--] = 0x00000000;
  while(i >= 0)
  {
    add = ((u64) pxb->addr.addr[i]) + 0x00000001;
    if(add < 0xFFFFFFFF)
    {
      pxa->addr.addr[i]  = (u32) add;
      pxa->addr = ipa_and(pxa->addr, ipa_mkmask(pxb->len));
      return;
    }
    pxa->addr.addr[i--] = 0x00000000;
  }

  pxa->addr = IPA_NONE;
}

/**
 * choose_prefix - Choose a prefix of specified length from
 * a usable prefix and a list of sub-prefixes in use
 * @pxu: The usable prefix
 * @px: A pointer to the prefix structure. Length must be set.
 * @used: The list of sub-prefixes already in use
 *
 * This function stores a unused prefix of specified length from
 * the usable prefix @pxu, and returns PXCHOOSE_SUCCESS,
 * or stores IPA_NONE into @px->ip and returns PXCHOOSE_FAILURE if
 * all prefixes are in use.
 */
static int
choose_prefix(struct prefix *pxu, struct prefix *px, list used, u32 rid, struct ospf_iface *ifa)
{
  /* (Stupid) Algorithm:
     - try a random prefix until success or 10 attempts have passed
     - if failure, do:
       * set looped to 0
       * store prefix in start_prefix
       * while looped is 0 or prefix is strictly smaller than start_prefix, do:
         * if prefix is not in usable prefix range, set to
           lowest prefix of range and set looped to 1
         * if prefix is available, return
         * find one of the used prefixes which contains/is contained in this prefix then
           increment prefix to the first prefix of correct length that
           is not covered by that used prefix / does not cover that used prefix */
  struct prefix_node *n;
  int looped;
  struct prefix start_prefix;

  int i;
  for(i=0;i<10;i++){
    random_prefix(pxu, px, rid, ifa, i);
    if(!in_use(px, used))
      return PXCHOOSE_SUCCESS;
  }

  looped = 0;
  start_prefix = *px;
  while(looped == 0 || ipa_compare(px->addr, start_prefix.addr) < 0)
  {
    if(!net_in_net(px->addr, px->len, pxu->addr, pxu->len))
    {
      px->addr = pxu->addr;
      looped = 1;
    }

    if(!in_use(px, used))
      return PXCHOOSE_SUCCESS;

    WALK_LIST(n, used)
    {
      if(net_in_net(px->addr, px->len, n->px.addr, n->px.len)
         || net_in_net(n->px.addr, n->px.len, px->addr, px->len))
      {
        next_prefix(px, &n->px);
        break;
      }
    }
  }
  return PXCHOOSE_FAILURE;
}

void
ospf_pxassign(struct proto_ospf *po)
{
  struct proto *p = &po->proto;
  struct ospf_area *oa;

  OSPF_TRACE(D_EVENTS, "Starting prefix assignment algorithm");

  WALK_LIST(oa, po->area_list)
  {
    // prefix assignment algorithm
    ospf_pxassign_area(oa);
  }
}

/**
 * ospf_pxassign_area - Run prefix assignment algorithm for
 * usable prefixes advertised by AC LSAs in a specific area.
 *
 * @oa: The area to search for LSAs in. Note that the algorithm
 * may impact interfaces that are not in this area.
 */
void
ospf_pxassign_area(struct ospf_area *oa)
{
  struct proto *p = &oa->po->proto;
  struct proto_ospf *po = oa->po;
  struct top_hash_entry *en;
  struct ospf_iface *ifa;
  struct prefix_node *asp;
  struct ospf_lsa_ac_tlv_v_usp *usp;
  int change = 0;

  //OSPF_TRACE(D_EVENTS, "Starting prefix assignment algorithm for AC LSAs in area %R", oa->areaid);

  /* mark all this area's iface's assignments as invalid */
  WALK_LIST(ifa, po->iface_list)
  {
    if(ifa->oa == oa)
    {
      WALK_LIST(asp, ifa->asp_list)
      {
        asp->valid = 0;
      }
    }
  }

  // perform the prefix assignment algorithm on each (USP, iface) tuple
  PARSE_LSA_AC_USP_START(usp,en)
  {
    WALK_LIST(ifa, po->iface_list)
    {
      if(ifa->oa == oa)
      {
        change |= ospf_pxassign_usp_ifa(ifa, (struct ospf_lsa_ac_tlv_v_usp *)(usp));
      }
    }
  }
  PARSE_LSA_AC_USP_END(en);

  /* remove all this area's iface's invalid assignments */
  WALK_LIST(ifa, po->iface_list)
  {
    if(ifa->oa == oa)
    {
      WALK_LIST(asp, ifa->asp_list)
      {
        if(!asp->valid)
        {
          if(asp->rid == po->router_id)
            change = 1;
          OSPF_TRACE(D_EVENTS, "Interface %s: assignment %I/%d removed as invalid", ifa->iface->name, asp->px.addr, asp->px.len);
          configure_ifa_del_prefix(asp, ifa);
        }
      }
    }
  }

  if(change)
  {
     schedule_ac_lsa(oa);
  }
}

/** ospf_pxassign_usp_ifa - Main prefix assignment algorithm
 *
 * @ifa: The Current Interface
 * @usp: The Current Usable Prefix
 */
int
ospf_pxassign_usp_ifa(struct ospf_iface *ifa, struct ospf_lsa_ac_tlv_v_usp *cusp)
{
  struct top_hash_entry *en;
  struct ospf_area *oa = ifa->oa;
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  //struct ospf_neighbor *neigh;
  //struct ospf_usp *usp;
  struct ospf_iface *ifa2;
  struct ospf_lsa_ac_tlv_v_usp *usp2;
  struct ospf_lsa_ac_tlv_v_asp *asp;
  struct ospf_lsa_ac_tlv_v_iasp *iasp;
  struct ospf_neighbor *neigh;
  struct prefix_node *pxn, *n, *self_r_px = NULL;
  //timer *pxassign_timer;
  ip_addr usp_addr, usp2_addr, neigh_addr, neigh_r_addr;
  unsigned int usp_len, usp2_len, neigh_len, neigh_r_len;
  u8 usp_pxopts, usp2_pxopts, neigh_pxopts;
  u16 usp_rest, usp2_rest, neigh_rest;
  int change = 0;

  lsa_get_ipv6_prefix((u32 *)cusp, &usp_addr, &usp_len, &usp_pxopts, &usp_rest);

  //OSPF_TRACE(D_EVENTS, "Starting prefix assignment algorithm for prefix %I/%d", ip, pxlen);

  /* 5.3.0 */
  PARSE_LSA_AC_USP_START(usp2, en)
  {
    lsa_get_ipv6_prefix((u32 *)usp2, &usp2_addr, &usp2_len, &usp2_pxopts, &usp2_rest);
    if(net_in_net(usp_addr, usp_len, usp2_addr, usp2_len) && (!ipa_equal(usp_addr, usp2_addr) || usp_len != usp2_len))
      return change;
  }
  PARSE_LSA_AC_USP_END(en);

  /* 5.3.1 */
  /* FIXME I think the draft should say "active neighbors" (state >= Init), that's what I suppose */
  /*int have_neigh = 0;
  WALK_LIST(neigh, ifa->neigh_list)
  {
    if(neigh->state >= NEIGHBOR_INIT)
      have_neigh = 1;
  }*/

  /* 5.3.2a */
  int have_highest_link_pa_priority = 0;
  u8 highest_link_pa_priority = 0;
  WALK_LIST(neigh, ifa->neigh_list)
  {
    if(neigh->state >= NEIGHBOR_INIT)
    {
      PARSE_LSA_AC_IASP_ROUTER_START(neigh->rid, iasp, en)
      {
        if(iasp->id == neigh->iface_id)
        {
          // store for future reference
          neigh->pa_priority = iasp->pa_priority;
          neigh->pa_pxlen = iasp->pa_pxlen;

          if(iasp->pa_priority > highest_link_pa_priority)
            highest_link_pa_priority = iasp->pa_priority;
        }
      }
      PARSE_LSA_AC_IASP_ROUTER_END(en);
    }
  }
  if(highest_link_pa_priority <= ifa->pa_priority)
  {
    highest_link_pa_priority = ifa->pa_priority;
    have_highest_link_pa_priority = 1;
  }

  /* 5.3.2b */
  int have_highest_rid = 1;
  WALK_LIST(neigh, ifa->neigh_list)
  {
    if(neigh->state >= NEIGHBOR_INIT && neigh->pa_priority == ifa->pa_priority && neigh->rid > po->router_id)
    {
      have_highest_rid = 0;
      break;
    }
  }

  /* 5.3.2c */
  int assignment_found = 0;
  u32 neigh_rid = 0;
  WALK_LIST(neigh, ifa->neigh_list)
  {
    if(neigh->pa_priority == highest_link_pa_priority && neigh->rid > neigh_rid && neigh->state >= NEIGHBOR_INIT)
    {
      PARSE_LSA_AC_IASP_ROUTER_START(neigh->rid, iasp, en)
      {
        if(iasp->id == neigh->iface_id)
        {
          PARSE_LSA_AC_ASP_START(asp, iasp)
          {
            lsa_get_ipv6_prefix((u32 *)(asp), &neigh_addr, &neigh_len, &neigh_pxopts, &neigh_rest);
            if(net_in_net(neigh_addr, neigh_len, usp_addr, usp_len))
            {
              /* a prefix has already been assigned by a neighbor to the link */
              /* we're not sure it is responsible for the link yet, so we store
                 the assigned prefix and keep looking at other neighbors with
                 same priority and higher RID */
              neigh_r_addr = neigh_addr;
              neigh_r_len = neigh_len;
              neigh_rid = neigh->rid;
              assignment_found = 1;
              break;
            }
          }
          PARSE_LSA_AC_ASP_BREAKIF(assignment_found);
        }
      }
      PARSE_LSA_AC_IASP_ROUTER_BREAKIF(assignment_found, en);
    }
  }

  /* 5.3.2d */
  int have_assignment_resp = 0;
  if(ifa->pa_priority == highest_link_pa_priority && po->router_id > neigh_rid)
  {
    struct prefix usp_px;
    usp_px.addr = usp_addr;
    usp_px.len = usp_len;
    self_r_px = assignment_find(ifa, &usp_px);
    if(self_r_px)
      have_assignment_resp = 1;
  }

  /* 5.3.3 */
  // exactly one of the following will be executed:
  // step 4 will be executed if:
  //   have_highest_link_pa_priority && have_assignment_resp
  // step 5 will be executed if:
  //   (!have_assignment_resp || !have_highest_link_pa_priority) && assignment_found
  // step 6 will be executed if:
  //   have_highest_link_pa_priority && have_highest_rid && !have_assignment_resp && !assignment_found
  if((!have_highest_link_pa_priority || (!have_assignment_resp && !have_highest_rid)) && !assignment_found)
    return change; // go to next interface

  /* 5.3.4 */
  // we already have an assignment but must check whether it is valid and whether there is better
  int deassigned_prefix = 0; // whether we had to remove our own assignment
  if(have_highest_link_pa_priority && have_assignment_resp)
  {
    PARSE_LSA_AC_IASP_START(iasp, en)
    {
      if(en->lsa.rt != po->router_id && iasp->pa_priority >= ifa->pa_priority)
      {  // don't check our own LSAs
        PARSE_LSA_AC_ASP_START(asp, iasp)
        {
          ip_addr addr;
          unsigned int len;
          u8 pxopts;
          u16 rest;

          lsa_get_ipv6_prefix((u32 *)(asp), &addr, &len, &pxopts, &rest);

          // test if assigned prefix collides with our assignment
          // 3 cases:
          //   same priority, assigned prefix is longer
          //   same priority, higher RID, same assigned prefix
          //   higher priority, any type of collision
          if((iasp->pa_priority == ifa->pa_priority && net_in_net(addr, len, self_r_px->px.addr, self_r_px->px.len)
              && (!ipa_equal(addr, self_r_px->px.addr) || len != self_r_px->px.len))
             || (iasp->pa_priority == ifa->pa_priority && en->lsa.rt > po->router_id
                 && ipa_equal(addr, self_r_px->px.addr) && len == self_r_px->px.len)
             || (iasp->pa_priority > ifa->pa_priority && (net_in_net(addr, len, self_r_px->px.addr, self_r_px->px.len)
                                                          || net_in_net(self_r_px->px.addr, self_r_px->px.len, addr, len))))
          {
            OSPF_TRACE(D_EVENTS, "Interface %s: assignment %I/%d collides with %I/%d, removing", ifa->iface->name, self_r_px->px.addr, self_r_px->px.len, addr, len);
            configure_ifa_del_prefix(self_r_px, ifa);
            deassigned_prefix = 1;
            change = 1;
            break;
          }
        }
        PARSE_LSA_AC_ASP_BREAKIF(deassigned_prefix);
      }
    }
    PARSE_LSA_AC_IASP_BREAKIF(deassigned_prefix, en);

    // also check our other assignments to see if this one is valid.
    // This should be useless: we should never have made a colliding assignment
    // without deleting this one in the first place
    if(!deassigned_prefix)
    {
      WALK_LIST(ifa2, po->iface_list)
      {
        if(ifa->oa == oa)
        {
          WALK_LIST(n, ifa2->asp_list)
          {
            if(n->rid == po->router_id)
            {
              // these 3 cases should be impossible:
              //   same priority, same RID, same assigned prefix, different interface
              //   same priority, same RID, strictly longer assigned prefix
              //   higher priority, same RID, any type of collision
              if((ifa2->pa_priority == ifa->pa_priority && net_in_net(n->px.addr, n->px.len, self_r_px->px.addr, self_r_px->px.len)
                  && (ipa_equal(n->px.addr, self_r_px->px.addr) && n->px.len == self_r_px->px.len) && ifa != ifa2)
                 || (ifa2->pa_priority == ifa->pa_priority && net_in_net(n->px.addr, n->px.len, self_r_px->px.addr, self_r_px->px.len)
                  && (!ipa_equal(n->px.addr, self_r_px->px.addr) || n->px.len != self_r_px->px.len))
                 || (ifa2->pa_priority > ifa->pa_priority && (net_in_net(n->px.addr, n->px.len, self_r_px->px.addr, self_r_px->px.len)
                                                              || net_in_net(self_r_px->px.addr, self_r_px->px.len, n->px.addr, n->px.len))))
              {
                die("Bug in prefix assignment algorithm: forgot to remove a prefix when assigning new one");
                /*OSPF_TRACE(D_EVENTS, "Interface %s: own assignment %I/%d collides with %I/%d, removing", ifa->iface->name, self_r_px->px.addr, self_r_px->px.len, addr, len);
                rem_node(NODE self_r_px);
                mb_free(self_r_px);
                deassigned_prefix = 1;
                change = 1;
                // FIXME deassign prefix from interface
                break;*/
              }
            }
          }
          // if(deassigned_prefix) break;
        }
      }
    }

    if(!deassigned_prefix && self_r_px->px.len == LSA_AC_ASP_SUB_PREFIX_LENGTH)
    {
      // Our assignment is valid. Still, if it is a /80 a /64 would be better.
      // If the prefix is a /80, check if we can assign any /64.
      // We can do that using re-using steps 5.3.6a through 5.3.6d
      // except we remove our /80 from the equation.

      list used; /* list of struct prefix_node */
      init_list(&used);
      ip_addr steal_addr, split_addr;
      unsigned int steal_len, split_len;
      unsigned int found_steal = 0, found_split = 0;
      unsigned int pxchoose_success = 0;
      u8 lowest_pa_priority;

      /* re-use 5.3.6a */
      // find all used prefixes in LSADB and our own interface's asp_lists,
      // excluding this /80
      find_used(ifa, usp_addr, usp_len, &used, &steal_addr, &steal_len, &found_steal,
                &split_addr, &split_len, &found_split, &lowest_pa_priority, self_r_px);

      /* re-use 5.3.6b */
      // see if we can find a /64 in memory that is unused
      try_reuse(ifa, usp_addr, usp_len, &used, &pxchoose_success, &change, LSA_AC_ASP_D_PREFIX_LENGTH, self_r_px);

      /* re-use 5.3.6c */
      // see if we can find an unused /64
      try_assign_unused(ifa, usp_addr, usp_len, &used, &pxchoose_success, &change, LSA_AC_ASP_D_PREFIX_LENGTH, self_r_px);

      /* re-use 5.3.6d */
      // try to steal a /64
      if(!pxchoose_success && found_steal) // try to steal a /64
      {
        try_steal(ifa, usp_addr, usp_len, &steal_addr, &steal_len, &found_steal,
                  &lowest_pa_priority, &pxchoose_success, &change, self_r_px);
      }

      WALK_LIST_DELSAFE(n, pxn, used)
      {
        rem_node(NODE n);
        mb_free(n);
      }

    }

    if(!deassigned_prefix)
    {
      self_r_px->valid = 1;
    }
  }

  /* 5.3.5 */
  // we must check whether we are aware of someone else's assignment
  if((!have_assignment_resp || !have_highest_link_pa_priority) && assignment_found)
  {
    int found = 0; // whether assignment is already in the ifa's asp_list
    WALK_LIST(n,ifa->asp_list)
    {
      if(ipa_equal(n->px.addr, neigh_r_addr) && n->px.len == neigh_r_len
         && n->rid == neigh_rid && n->pa_priority == highest_link_pa_priority)
      {
        found = 1;
        n->valid = 1;
      }
    }

    // if it's not already there, we must run some extra checks to see if we can assign it.
    // parse all interface's asp_lists twice: once to determine if the new assignment takes
    // priority, second to remove all colliding assignments if it does
    // cases an existing assignment wins and new one must be refused:
    //   existing has a strictly higher pa_priority
    //   existing has the same pa_priority and a strictly longer prefix
    //   existing has the same pa_priority, same prefix and higher RID
    int refused = 0;
    int collision_found = 0;
    if(!found)
    {
      WALK_LIST(ifa2, po->iface_list)
      {
        if(ifa2->oa == oa)
        {
          WALK_LIST(n, ifa2->asp_list)
          {
            if(net_in_net(n->px.addr, n->px.len, neigh_r_addr, neigh_r_len)
               || net_in_net(neigh_r_addr, neigh_r_len, n->px.addr, n->px.len))
            {
              collision_found = 1;
              if(n->pa_priority > highest_link_pa_priority
                 || (n->pa_priority == highest_link_pa_priority && net_in_net(n->px.addr, n->px.len, neigh_r_addr, neigh_r_len)
                     && (!ipa_equal(neigh_r_addr, n->px.addr) || neigh_r_len != n->px.len))
                 || (n->pa_priority == highest_link_pa_priority && (ipa_equal(neigh_r_addr, n->px.addr) && neigh_r_len == n->px.len)
                     && po->router_id > neigh_rid))
              {
                refused = 1;
                OSPF_TRACE(D_EVENTS, "Interface %s: Refused %R's assignment %I/%d with priority %d, we have interface %s assignment %I/%d with priority %d",
                                     ifa->iface->name, neigh_rid, neigh_r_addr, neigh_r_len, highest_link_pa_priority, ifa2->iface->name, n->px.addr, n->px.len, n->pa_priority);
                break;
                // we will have no assignment on this interface, but we don't know who's responsible.
                // this might be a vulnerability: if the neighbor is ill-intentioned and
                // never removes his assignment, no prefix will ever be assigned on this interface.
                // it would be possible to run some additional steps to see if we are responsible here.
                // under normal conditions, the neighbor will eventually remove his assignment.
              }
            }
          }
          if(refused) break;
        }
      }
    }
    if(!refused && collision_found)
    {
      // delete all colliding assignments on interfaces
      WALK_LIST(ifa2, po->iface_list)
      {
        if(ifa2->oa == oa)
        {
          WALK_LIST_DELSAFE(n, pxn, ifa2->asp_list)
          {
            if(net_in_net(n->px.addr, n->px.len, neigh_r_addr, neigh_r_len)
               || net_in_net(neigh_r_addr, neigh_r_len, n->px.addr, n->px.len))
            {
              OSPF_TRACE(D_EVENTS, "Interface %s: To add %R's assignment %I/%d with priority %d, must delete interface %s assignment %I/%d with priority %d",
                                   ifa->iface->name, neigh_rid, neigh_r_addr, neigh_r_len, highest_link_pa_priority, ifa2->iface->name, n->px.addr, n->px.len, n->pa_priority);
              if(n->rid == po->router_id)
                change = 1;
              configure_ifa_del_prefix(n, ifa2);
            }
          }
        }
      }
    }

    if(!found && !refused)
    {
      OSPF_TRACE(D_EVENTS, "Interface %s: Adding %R's assignment %I/%d with priority %d", ifa->iface->name, neigh_rid, neigh_r_addr, neigh_r_len, highest_link_pa_priority);
      configure_ifa_add_prefix(neigh_r_addr, neigh_r_len, neigh_rid, po->router_id, highest_link_pa_priority, ifa);
    }
  }

  /* 5.3.6 */
  // we must assign a new prefix
  if(deassigned_prefix
     || (have_highest_link_pa_priority && !have_assignment_resp && !assignment_found && have_highest_rid))
  {
    list used; /* list of struct prefix_node */
    init_list(&used);
    ip_addr steal_addr, split_addr;
    unsigned int steal_len, split_len;
    unsigned int found_steal = 0, found_split = 0;
    unsigned int pxchoose_success = 0;
    u8 lowest_pa_priority;

    /* 5.3.6a */
    // find all used prefixes in LSADB and our own interface's asp_lists
    find_used(ifa, usp_addr, usp_len, &used, &steal_addr, &steal_len, &found_steal,
              &split_addr, &split_len, &found_split, &lowest_pa_priority, NULL);

    /* 5.3.6b */
    // see if we can find a /64 in memory that is unused
    try_reuse(ifa, usp_addr, usp_len, &used, &pxchoose_success, &change, LSA_AC_ASP_D_PREFIX_LENGTH, NULL);

    /* 5.3.6c */
    // see if we can find an unused /64
    try_assign_unused(ifa, usp_addr, usp_len, &used, &pxchoose_success, &change, LSA_AC_ASP_D_PREFIX_LENGTH, NULL);

    /* 5.3.6d */
    // try to steal a /64
    if(!pxchoose_success && found_steal)
    {
      try_steal(ifa, usp_addr, usp_len, &steal_addr, &steal_len, &found_steal,
                &lowest_pa_priority, &pxchoose_success, &change, NULL);
    }

    /* 5.3.6e */
    // see if we can find a /80 in memory that is unused
    if(!pxchoose_success && ifa->pa_priority < PA_PRIORITY_MAX)
      try_reuse(ifa, usp_addr, usp_len, &used, &pxchoose_success, &change, LSA_AC_ASP_SUB_PREFIX_LENGTH, NULL);

    /* 5.3.6f */
    // see if we can find an unused /80
    if(!pxchoose_success && ifa->pa_priority < PA_PRIORITY_MAX)
      try_assign_unused(ifa, usp_addr, usp_len, &used, &pxchoose_success, &change, LSA_AC_ASP_SUB_PREFIX_LENGTH, NULL);

    /* 5.3.6g */
    // try to split a /64
    if(!pxchoose_success && found_split && ifa->pa_priority < PA_PRIORITY_MAX)
    {
      try_split(ifa, usp_addr, usp_len, &split_addr, &split_len,
          &found_split, &pxchoose_success, &change, LSA_AC_ASP_SUB_PREFIX_LENGTH);
    }

    /* 5.3.6h */
    if(!pxchoose_success)
      OSPF_TRACE(D_EVENTS, "Interface %s: No prefixes left to assign from prefix %I/%d.", ifa->iface->name, usp_addr, usp_len);

    WALK_LIST_DELSAFE(n, pxn, used)
    {
      rem_node(NODE n);
      mb_free(n);
    }
  }

  return change;
}

/**
 * find_used - Find all already used prefixes
 *
 * Updates list of used prefixes @used.
 * Also updates @steal_addr, @steal_len, @found_steal, @split_addr, @split_len, @found_split, @lowest_pa_priority.
 * If @self_r_px is not set to NULL, takes this prefix out of consideration (this is used when trying to determine
 * if a /80 can be transformed into a /64 because it is the only one using part of the /64).
 */
static void
find_used(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, list *used, ip_addr *steal_addr, unsigned int *steal_len,
          unsigned int *found_steal, ip_addr *split_addr, unsigned int *split_len, unsigned int *found_split, u8 *lowest_pa_priority,
          struct prefix_node *self_r_px)
{
  struct ospf_area *oa = ifa->oa;
  struct proto_ospf *po = oa->po;
  struct top_hash_entry *en;
  struct prefix_node *n, *pxn;
  struct ospf_lsa_ac_tlv_v_iasp *iasp;
  struct ospf_lsa_ac_tlv_v_asp *asp;
  struct ospf_iface *ifa2;

  *lowest_pa_priority = ifa->pa_priority;

  PARSE_LSA_AC_IASP_START(iasp, en)
  {
    if(en->lsa.rt != po->router_id) // don't check our own LSAs
    {
      PARSE_LSA_AC_ASP_START(asp, iasp)
      {
        ip_addr addr;
        unsigned int len;
        u8 pxopts;
        u16 rest;

        lsa_get_ipv6_prefix((u32 *)(asp) , &addr, &len, &pxopts, &rest);
        // test if assigned prefix is part of current usable prefix
        if(net_in_net(addr, len, usp_addr, usp_len))
        {
          /* add prefix to list of used prefixes */
          pxn = mb_alloc(ifa->pool, sizeof(struct prefix_node));
          add_tail(used, NODE pxn);
          pxn->px.addr = addr;
          pxn->px.len = len;
          pxn->pa_priority = iasp->pa_priority;
          pxn->rid = en->lsa.rt;

          // test if assigned prefix is stealable
          if(iasp->pa_priority < *lowest_pa_priority)
          {
            *steal_addr = ipa_and(addr,ipa_mkmask(LSA_AC_ASP_D_PREFIX_LENGTH));
            *steal_len = LSA_AC_ASP_D_PREFIX_LENGTH;
            *lowest_pa_priority = iasp->pa_priority;
            *found_steal = 1;
          }

          // test if assigned prefix is splittable
          if(!*found_split && iasp->pa_priority == ifa->pa_priority && len == LSA_AC_ASP_D_PREFIX_LENGTH)
          {
            *split_addr = addr;
            *split_len = len;
            *found_split = 1;
          }
        }
      }
      PARSE_LSA_AC_ASP_END;
    }
  }
  PARSE_LSA_AC_IASP_END(en);

  /* we also check our own interfaces for assigned prefixes */
  WALK_LIST(ifa2, po->iface_list)
  {
    if(ifa2->oa == oa)
    {
      WALK_LIST(n, ifa2->asp_list)
      {
        if(n->rid == po->router_id && net_in_net(n->px.addr, n->px.len, usp_addr, usp_len))
        {
          // check we are not considering the /80 we already assigned, if any
          if(!self_r_px
             || (!ipa_equal(self_r_px->px.addr, n->px.addr) || self_r_px->px.len != n->px.len)
             || ifa != ifa2)
          {
            /* add prefix to list of used prefixes */
            pxn = mb_alloc(ifa->pool, sizeof(struct prefix_node));
            add_tail(used, NODE pxn);
            pxn->px.addr = n->px.addr;
            pxn->px.len = n->px.len;
            pxn->rid = n->rid;
            pxn->pa_priority = ifa2->pa_priority;

            // test if assigned prefix is stealable
            if(ifa2->pa_priority < *lowest_pa_priority)
            {
              *steal_addr = ipa_and(n->px.addr,ipa_mkmask(LSA_AC_ASP_D_PREFIX_LENGTH));
              *steal_len = LSA_AC_ASP_D_PREFIX_LENGTH;
              *lowest_pa_priority = ifa2->pa_priority;
              *found_steal = 1;
            }

            // test if assigned prefix is splittable
            if(!*found_split && ifa2->pa_priority == ifa->pa_priority && n->px.len == LSA_AC_ASP_D_PREFIX_LENGTH)
            {
              *split_addr = n->px.addr;
              *split_len = n->px.len;
              *found_split = 1;
            }
          }
        }
      }
    }
  }
}

/**
 * try_reuse - Try to reuse an unused prefix of specified @length in memory
 */
static void
try_reuse(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, list *used,
            unsigned int *pxchoose_success, unsigned int *change, unsigned int length, struct prefix_node *self_r_px)
{
  // FIXME implement
}

/**
 * try_assign_unused - Try to assign an unused prefix of specified @length.
 * If @self_r_px is not set to NULL and a successful assignment takes place,
 * removes this prefix (this is used when trying to transform a /80 into a /64).
 * @length: The desired prefix length.
 */
static void
try_assign_unused(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, list *used,
                  unsigned int *pxchoose_success, unsigned int *change, unsigned int length, struct prefix_node *self_r_px)
{
  struct proto_ospf *po = ifa->oa->po;
  struct proto *p = &po->proto;

  struct prefix px_tmp, pxu_tmp;
  px_tmp.addr = IPA_NONE;
  px_tmp.len = length;
  pxu_tmp.addr = usp_addr;
  pxu_tmp.len = usp_len;
  switch(choose_prefix(&pxu_tmp, &px_tmp, *used, po->router_id, ifa))
  {
    case PXCHOOSE_SUCCESS:
      if(self_r_px)
      {
        // delete the old /80 that is going to be replaced
        OSPF_TRACE(D_EVENTS, "Interface %s: Replacing prefix %I/%d with prefix %I/%d from usable prefix %I/%d", ifa->iface->name, self_r_px->px.addr, self_r_px->px.len, px_tmp.addr, px_tmp.len, usp_addr, usp_len);
        configure_ifa_del_prefix(self_r_px, ifa);
      }
      else {
        OSPF_TRACE(D_EVENTS, "Interface %s: Chose prefix %I/%d to assign from usable prefix %I/%d", ifa->iface->name, px_tmp.addr, px_tmp.len, usp_addr, usp_len);
      }
      *change = 1;
      *pxchoose_success = 1;
      configure_ifa_add_prefix(px_tmp.addr, px_tmp.len, po->router_id, po->router_id, ifa->pa_priority, ifa);
      break;

    case PXCHOOSE_FAILURE:
      //log(L_WARN "%s: No prefixes left to assign to interface %s from prefix %I/%d.", p->name, ifa->iface->name, usp_addr, usp_len);
      break;
  }
}

/**
 * try_steal - Try to steal a used prefix
 */
static void
try_steal(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, ip_addr *steal_addr, unsigned int *steal_len,
          unsigned int *found_steal, u8 *lowest_pa_priority, unsigned int *pxchoose_success, unsigned int *change,
          struct prefix_node *self_r_px)
{
  struct ospf_area *oa = ifa->oa;
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  struct ospf_lsa_ac_tlv_v_iasp *iasp;
  struct ospf_lsa_ac_tlv_v_asp *asp;
  struct top_hash_entry *en;
  struct ospf_iface *ifa2;
  struct prefix_node *n, *pxn;

  // we need to check that no one else has already stolen/split the stealable prefix.
  // Policy is only steal if no one with strictly higher than lowest pa_priority
  // has already stolen (conservative policy)
  PARSE_LSA_AC_IASP_START(iasp, en)
  {
    if(en->lsa.rt != po->router_id && iasp->pa_priority > *lowest_pa_priority)
    {  // don't check our own LSAs
      PARSE_LSA_AC_ASP_START(asp, iasp)
      {
        ip_addr addr;
        unsigned int len;
        u8 pxopts;
        u16 rest;

        lsa_get_ipv6_prefix((u32 *)(asp) , &addr, &len, &pxopts, &rest);
        if((net_in_net(addr, len, *steal_addr, *steal_len)
            || net_in_net(*steal_addr, *steal_len, addr, len)))
          *found_steal = 0;
      }
      PARSE_LSA_AC_ASP_BREAKIF(!*found_steal);
    }
  }
  PARSE_LSA_AC_IASP_BREAKIF(!*found_steal, en);

  // we also need to check that we have not already stolen/split the prefix
  // ourselves...
  if(*found_steal)
  {
    WALK_LIST(ifa2, po->iface_list)
    {
      if(ifa2->oa == oa)
      {
        WALK_LIST(n, ifa2->asp_list)
        {
          if(n->rid == po->router_id)
          {
            if((net_in_net(n->px.addr, n->px.len, *steal_addr, *steal_len)
                || net_in_net(*steal_addr, *steal_len, n->px.addr, n->px.len))
               // check we are not considering the /80 we already assigned, if any
               && (!self_r_px
                   || (!ipa_equal(self_r_px->px.addr, n->px.addr) || self_r_px->px.len != n->px.len)
                   || ifa != ifa2))
            {
              if(ifa2->pa_priority > *lowest_pa_priority)
                *found_steal = 0;
            }
          }
        }
      }
    }
  }

  // this is where we know we can do the assignment
  if(*found_steal)
  {
    // delete colliding assignments from any other interfaces
    WALK_LIST(ifa2, po->iface_list)
    {
      if(ifa2->oa == oa)
      {
        WALK_LIST_DELSAFE(n, pxn, ifa2->asp_list)
        {
          if((net_in_net(n->px.addr, n->px.len, *steal_addr, *steal_len)
              || net_in_net(*steal_addr, *steal_len, n->px.addr, n->px.len))
             // we will delete the /80 just after, if any
             && (!self_r_px
                 || (!ipa_equal(self_r_px->px.addr, n->px.addr) || self_r_px->px.len != n->px.len
                     || n->rid != po->router_id)))
          {
            OSPF_TRACE(D_EVENTS, "Interface %s: stealing assignment %I/%d, must remove %I/%d from interface %s", ifa->iface->name, *steal_addr, *steal_len, n->px.addr, n->px.len, ifa2->iface->name);
            if(n->rid == po->router_id)
              *change = 1;
            configure_ifa_del_prefix(n, ifa2);
          }
        }
      }
    }

    // finally, steal the prefix
    if(self_r_px)
    {
      OSPF_TRACE(D_EVENTS, "Interface %s: Replacing prefix %I/%d with stolen prefix %I/%d from usable prefix %I/%d", ifa->iface->name, self_r_px->px.addr, self_r_px->px.len, *steal_addr, *steal_len, usp_addr, usp_len);
      configure_ifa_del_prefix(self_r_px, ifa);
    }
    else {
      OSPF_TRACE(D_EVENTS, "Interface %s: stole prefix %I/%d to assign from usable prefix %I/%d", ifa->iface->name, *steal_addr, *steal_len, usp_addr, usp_len);
    }
    *change = 1;
    *pxchoose_success = 1;
    configure_ifa_add_prefix(*steal_addr, *steal_len, po->router_id, po->router_id, ifa->pa_priority, ifa);
  }
}

/**
 * try_split - Try to split a used prefix of into a prefix of specified @length
 */
static void
try_split(struct ospf_iface *ifa, ip_addr usp_addr, unsigned int usp_len, ip_addr *split_addr, unsigned int *split_len,
          unsigned int *found_split, unsigned int *pxchoose_success, unsigned int *change, unsigned int length)
{
  struct ospf_area *oa = ifa->oa;
  struct proto_ospf *po = oa->po;
  struct proto *p = &po->proto;
  struct ospf_lsa_ac_tlv_v_iasp *iasp;
  struct ospf_lsa_ac_tlv_v_asp *asp;
  struct top_hash_entry *en;
  struct ospf_iface *ifa2;
  struct prefix_node *n, *pxn;

  // we need to check that no one else has already stolen/split this prefix.
  // Policy is only split if no one with our priority has already split
  // and no one with a strictly higher priority collides
  PARSE_LSA_AC_IASP_START(iasp, en)
  {
    if(en->lsa.rt != po->router_id && iasp->pa_priority >= ifa->pa_priority)
    {  // don't check our own LSAs
      PARSE_LSA_AC_ASP_START(asp, iasp)
      {
        ip_addr addr;
        unsigned int len;
        u8 pxopts;
        u16 rest;

        lsa_get_ipv6_prefix((u32 *)(asp) , &addr, &len, &pxopts, &rest);
        if(iasp->pa_priority > ifa->pa_priority && (net_in_net(addr, len, *split_addr, *split_len)
                                                    || net_in_net(*split_addr, *split_len, addr, len)))
          found_split = 0;
        if(iasp->pa_priority == ifa->pa_priority && (net_in_net(addr, len, *split_addr, *split_len))
                                                 && (!ipa_equal(addr, *split_addr) || len != *split_len))
          found_split = 0;
      }
      PARSE_LSA_AC_ASP_BREAKIF(!*found_split);
    }
  }
  PARSE_LSA_AC_IASP_BREAKIF(!*found_split, en);

  // we also need to check that we have not already stolen/split the prefix
  // ourselves and not had time to put it in LSADB...
  if(*found_split)
  {
    WALK_LIST(ifa2, po->iface_list)
    {
      if(ifa2->oa == oa)
      {
        WALK_LIST(n, ifa2->asp_list)
        {
          if(n->rid == po->router_id)
          {
            if(ifa2->pa_priority > ifa->pa_priority && (net_in_net(n->px.addr, n->px.len, *split_addr, *split_len)
                                                         || net_in_net(*split_addr, *split_len, n->px.addr, n->px.len)))
              *found_split = 0;
            if(ifa2->pa_priority == ifa->pa_priority && (net_in_net(n->px.addr, n->px.len, *split_addr, *split_len))
                                                     && (!ipa_equal(n->px.addr, *split_addr) || n->px.len != *split_len))
              *found_split = 0;
            if(!*found_split) break;
          }
        }
        if(!*found_split) break;
      }
    }
  }

  // this is where we know we can do the assignment
  if(*found_split)
  {
    // delete colliding assignments from any other interfaces
    WALK_LIST(ifa2, po->iface_list)
    {
      if(ifa2->oa == oa)
      {
        WALK_LIST_DELSAFE(n, pxn, ifa2->asp_list)
        {
          if(net_in_net(n->px.addr, n->px.len, *split_addr, *split_len)
             || net_in_net(*split_addr, *split_len, n->px.addr, n->px.len))
          {
            OSPF_TRACE(D_EVENTS, "Interface %s: splitting assignment %I/%d, must remove %I/%d from interface %s", ifa->iface->name, *split_addr, *split_len, n->px.addr, n->px.len, ifa2->iface->name);
            if(n->rid == po->router_id)
              *change = 1;
            configure_ifa_del_prefix(n, ifa2);
          }
        }
      }
    }

    // finally, split the prefix
    struct prefix px_tmp, pxu_tmp;
    px_tmp.addr = IPA_NONE;
    px_tmp.len = length;
    pxu_tmp.addr = *split_addr;
    pxu_tmp.len = *split_len;
    list empty_list;
    init_list(&empty_list);
    switch(choose_prefix(&pxu_tmp, &px_tmp, empty_list, po->router_id, ifa))
    {
      case PXCHOOSE_SUCCESS:
        OSPF_TRACE(D_EVENTS, "Interface %s: split prefix %I/%d to assign from usable prefix %I/%d", ifa->iface->name, px_tmp.addr, px_tmp.len, usp_addr, usp_len);
        *change = 1;
        *pxchoose_success = 1;
        configure_ifa_add_prefix(pxu_tmp.addr, pxu_tmp.len, po->router_id, po->router_id, ifa->pa_priority, ifa);
        break;
      case PXCHOOSE_FAILURE: //impossible
        die("bug in prefix assignment algorithm");
        break;
    }
  }
}

#ifdef ENABLE_SYSEVENT
#define USABLE_PREFIX_LENGTH (STD_ADDRESS_P_LENGTH + 4) /* 39 for IPv6 address, 4 for /length */
static char usable_prefix[USABLE_PREFIX_LENGTH];
#endif

int
update_dhcpv6_usable_prefix(struct proto_ospf *po)
{
#ifdef ENABLE_SYSEVENT
  struct proto *p = &po->proto;
  struct prefix_node pxn;
  struct prefix_node *n;
  struct ospf_area *oa;
  char *pos;
  int have_dhcp_usp = 1;
  int found = 0;
  int change = 0;

  if (bird_sysevent_get(NULL, "ipv6_delegated_prefix", usable_prefix, USABLE_PREFIX_LENGTH) == -1)
  {
    have_dhcp_usp = 0;
  }
  else if ((pos=strchr(usable_prefix, '/')) != NULL)
  {
    *pos = '\0';
    if(ip_pton(usable_prefix, &pxn.px.addr))
    {
      pxn.px.len = atoi(pos + 1);
      pxn.type = OSPF_USP_T_DHCPV6;
    }
    else have_dhcp_usp = 0;
  }
  else have_dhcp_usp = 0;

  // update usp_list entries of type DHCPV6
  WALK_LIST(n, po->usp_list)
  {
    if(n->type == OSPF_USP_T_DHCPV6)
    {
      if(!have_dhcp_usp || !ipa_equal(n->px.addr, pxn.px.addr) || n->px.len != pxn.px.len)
      {
        // remove this node
        OSPF_TRACE(D_EVENTS, "Removing DHCPv6 prefix: %I/%d", n->px.addr, n->px.len);
        rem_node(NODE n);
        mb_free(n);
        change = 1;
      }
      else found = 1;
    }
  }
  if(have_dhcp_usp && !found)
  {
    OSPF_TRACE(D_EVENTS, "Found new DHCPv6 prefix: %I/%d", pxn.px.addr, pxn.px.len);
    ospf_usp_add(po, &pxn);
    change = 1;
  }
  if(change)
  {
    WALK_LIST(oa, po->area_list)
      schedule_ac_lsa(oa);
  }
#endif /* ENABLE_SYSEVENT */
  return 0;
}

void
ospf_pxassign_reconfigure_iface(struct ospf_iface *ifa)
{
  struct prefix_node *n;
  struct proto_ospf *po = ifa->oa->po;

  WALK_LIST(n, ifa->asp_list)
  {
    if(n->rid == po->router_id)
    {
      n->pa_priority = ifa->pa_priority;
    }
  }
}
#endif /* OSPFv3 */
