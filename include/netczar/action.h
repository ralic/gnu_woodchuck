#ifndef NETCZAR_ACTION_H
#define NETCZAR_ACTION_H

uint64_t netczar_action_register (netczar_action_t action);

int netczar_action_deregister (uint64_t identifier)

// list (application, action_class)

struct netczar_action;
typedef struct netczar_action *netczar_action_t;

/* Allocate a new, default action.  */
netczar_action_t netczar_action_new (void);

/* Free a action.  */
void netczar_action_free (netczar_action_t action);

/* Copy a action.  Both the original and the new must be freed using
   netczar_action_free.  */
netczar_action_t netczar_action_copy (const netczar_action_t action);




#endif
