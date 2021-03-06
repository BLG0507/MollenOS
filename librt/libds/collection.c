/**
 * MollenOS
 *
 * Copyright 2011 - 2018, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * MollenOS - Generic Collection Implementation
 *  - Implements Collection and queue functionality
 */

#include <ds/collection.h>
#include <ddk/io.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>

void
CollectionConstruct(
    _In_ Collection_t* Collection,
    _In_ KeyType_t     KeyType)
{
    memset(Collection, 0, sizeof(Collection_t));
    Collection->KeyType = KeyType;
}

Collection_t*
CollectionCreate(
    _In_ KeyType_t KeyType)
{
    Collection_t *Collection = (Collection_t*)dsalloc(sizeof(Collection_t));
    if (!Collection) {
        return NULL;
    }
    
    CollectionConstruct(Collection, KeyType);
    return Collection;
}

OsStatus_t
CollectionClear(
    _In_ Collection_t* Collection)
{
    CollectionItem_t *Node = NULL;
    assert(Collection != NULL);

    // Get initial node and then
    // just iterate while destroying nodes
    Node = CollectionPopFront(Collection);
    while (Node != NULL) {
        CollectionDestroyNode(Collection, Node);
        Node = CollectionPopFront(Collection);
    }
    return OsSuccess;
}

OsStatus_t
CollectionDestroy(
    _In_ Collection_t* Collection)
{
    OsStatus_t Status = CollectionClear(Collection);
    if (Status != OsInvalidParameters) {
        dsfree(Collection);
    }
    return Status;
}

size_t
CollectionLength(
    _In_ Collection_t* Collection)
{
    assert(Collection != NULL);
    return atomic_load(&Collection->Length);
}

CollectionIterator_t*
CollectionBegin(
    _In_ Collection_t* Collection)
{
    assert(Collection != NULL);
    return Collection->Head;
}

CollectionIterator_t*
CollectionNext(
    _In_ CollectionIterator_t* It)
{
    return (It == NULL) ? NULL : It->Link;
}

/* CollectionCreateNode
 * Instantiates a new Collection node that can be appended to the Collection 
 * by CollectionAppend. If using an unsorted Collection set the sortkey == key */
CollectionItem_t*
CollectionCreateNode(
    _In_ DataKey_t              Key,
    _In_ void*                  Data)
{
    // Allocate a new instance of the Collection-node
    CollectionItem_t *Node = (CollectionItem_t*)dsalloc(sizeof(CollectionItem_t));
    memset(Node, 0, sizeof(CollectionItem_t));
    Node->Key       = Key;
    Node->Data      = Data;
    Node->Dynamic   = true;
    return Node;
}

/* CollectionDestroyNode
 * Cleans up a Collection node and frees all resources it had */
OsStatus_t
CollectionDestroyNode(
    _In_ Collection_t*          Collection,
    _In_ CollectionItem_t*      Node)
{
    assert(Collection != NULL);
    assert(Node != NULL);
    if (Node->Dynamic == false) {
        return OsSuccess;
    }

    // Behave different based on the type of key
    switch (Collection->KeyType) {
        case KeyString:
            dsfree((void*)Node->Key.Value.String.Pointer);
            break;

        default:
            break;
    }

    // Cleanup node and return
    dsfree(Node);
    return OsSuccess;
}

OsStatus_t
CollectionInsert(
    _In_ Collection_t*     Collection, 
    _In_ CollectionItem_t* Node)
{
    assert(Collection != NULL);
    assert(Node != NULL);

    Node->Prev = NULL;

    // In case the Collection is empty - no processing needed
    dslock(&Collection->SyncObject);
    if (!Collection->Head) {
        Node->Link       = NULL;
        Collection->Tail = Node;
        Collection->Head = Node;
    }
    else {
        Node->Link              = Collection->Head;
        Collection->Head->Prev  = Node;
        Collection->Head        = Node;
    }
    atomic_fetch_add(&Collection->Length, 1);
    dsunlock(&Collection->SyncObject);
    return OsSuccess;
}

OsStatus_t
CollectionAppend(
    _In_ Collection_t*     Collection,
    _In_ CollectionItem_t* Node)
{
    assert(Collection != NULL);
    assert(Node != NULL);

    Node->Link = NULL;

    // In case of empty Collection just update head/tail
    dslock(&Collection->SyncObject);
    smp_rmb();
    if (Collection->Head == NULL) {
        Node->Prev       = NULL;
        Collection->Tail = Node;
        Collection->Head = Node;
    }
    else {
        // Append to tail
        Node->Prev              = Collection->Tail;
        Collection->Tail->Link  = Node;
        Collection->Tail        = Node;
    }
    atomic_fetch_add(&Collection->Length, 1);
    smp_wmb();
    dsunlock(&Collection->SyncObject);
    return OsSuccess;
}

CollectionItem_t*
CollectionPopFront(
    _In_ Collection_t* Collection)
{
    CollectionItem_t *Current;
    
    assert(Collection != NULL);

    // Manipulate the Collection to find the next pointer of the
    // node that comes before the one to be removed.
    dslock(&Collection->SyncObject);
    smp_rmb();
    if (!Collection->Head) {
        dsunlock(&Collection->SyncObject);
        return NULL;
    }
    
    Current          = Collection->Head;
    Collection->Head = Current->Link;

    // Set previous to null
    if (Collection->Head != NULL) {
        Collection->Head->Prev = NULL;
    }

    // Update tail if necessary
    if (Collection->Tail == Current) {
        Collection->Tail = NULL;
    }
    atomic_fetch_sub(&Collection->Length, 1);
    smp_wmb();
    dsunlock(&Collection->SyncObject);

    // Reset its link (remove any Collection traces!)
    Current->Link = NULL;
    Current->Prev = NULL;
    return Current;
}

CollectionItem_t*
CollectionGetNodeByKey(
    _In_ Collection_t* Collection,
    _In_ DataKey_t     Key, 
    _In_ int           n)
{
    int               Counter = n;
    CollectionItem_t* i;
    assert(Collection != NULL);
    
    dslock(&Collection->SyncObject);
    if (!Collection->Head) {
        dsunlock(&Collection->SyncObject);
        return NULL;
    }

    _foreach(i, Collection) {
        if (!dsmatchkey(Collection->KeyType, i->Key, Key)) {
            if (Counter == 0) {
                break;
            }
            Counter--;
        }
    }
    dsunlock(&Collection->SyncObject);
    return Counter == 0 ? i : NULL;
}

void*
CollectionGetDataByKey(
    _In_ Collection_t* Collection, 
    _In_ DataKey_t     Key, 
    _In_ int           n)
{
    CollectionItem_t* Node = CollectionGetNodeByKey(Collection, Key, n);
    return (Node == NULL) ? NULL : Node->Data;
}

void
CollectionExecuteOnKey(
    _In_ Collection_t* Collection, 
    _In_ void          (*Function)(void*, int, void*), 
    _In_ DataKey_t     Key, 
    _In_ void*         Context)
{
    int               i = 0;
    CollectionItem_t* Node;
    assert(Collection != NULL);

    dslock(&Collection->SyncObject);
    _foreach(Node, Collection) {
        if (!dsmatchkey(Collection->KeyType, Node->Key, Key)) {
            Function(Node->Data, i++, Context);
        }
    }
    dsunlock(&Collection->SyncObject);
}

void
CollectionExecuteAll(
    _In_ Collection_t* Collection, 
    _In_ void          (*Function)(void*, int, void*), 
    _In_ void*         Context)
{
    int               i = 0;
    CollectionItem_t* Node;
    assert(Collection != NULL);

    dslock(&Collection->SyncObject);
    _foreach(Node, Collection) {
        Function(Node->Data, i++, Context);
    }
    dsunlock(&Collection->SyncObject);
}

CollectionItem_t*
CollectionSplice(
    _In_ Collection_t* Collection,
    _In_ int           Count)
{
    CollectionItem_t* InitialNode;
    int               NumberOfNodes;
    
    assert(Collection != NULL);
    
    dslock(&Collection->SyncObject);
    InitialNode   = Collection->Head;
    NumberOfNodes = MIN(atomic_load(&Collection->Length), Count);
    
    atomic_fetch_sub(&Collection->Length, NumberOfNodes);
    while (NumberOfNodes--) {
        Collection->Head = Collection->Head->Link;
    }
    
    if (Collection->Head == NULL) {
        Collection->Tail = NULL;
    }
    dsunlock(&Collection->SyncObject);
    return InitialNode;
}

static void
__collection_remove_node(
    _In_ Collection_t*     Collection, 
    _In_ CollectionItem_t* Node)
{
    if (Node->Prev == NULL) {
        // Ok, so this means we are the
        // first node in the Collection. Do we have a link?
        if (Node->Link == NULL) {
            // We're the only link
            // but lets stil validate we're from this Collection
            if (Collection->Head == Node) {
                Collection->Head = Collection->Tail = NULL;
                atomic_fetch_sub(&Collection->Length, 1);
            }
        }
        else {
            // We have a link this means we set headp to next
            if (Collection->Head == Node) {
                Collection->Head = Node->Link;
                Collection->Head->Prev = NULL;
                atomic_fetch_sub(&Collection->Length, 1);
            }
        }
    }
    else {
        // We have a previous,
        // Special case 1: we are last element
        // which means we should update pointer
        if (Node->Link == NULL) {
            // Ok, we are last element 
            // Update tail pointer to previous
            if (Collection->Tail == Node) {
                Collection->Tail = Node->Prev;
                Collection->Tail->Link = NULL;
                atomic_fetch_sub(&Collection->Length, 1);
            }
        }
        else {
            // Normal case, we just skip this
            // element without interfering with the Collection
            // pointers
            CollectionItem_t *Prev = Node->Prev;
            Prev->Link = Node->Link;
            Prev->Link->Prev = Prev;
            atomic_fetch_sub(&Collection->Length, 1);
        }
    }
}

CollectionItem_t*
CollectionUnlinkNode(
    _In_ Collection_t*     Collection, 
    _In_ CollectionItem_t* Node)
{
    CollectionItem_t* Item;
    assert(Collection != NULL);
    assert(Node != NULL);

    dslock(&Collection->SyncObject);
    __collection_remove_node(Collection, Node);
    Item = (Node->Prev == NULL) ? Collection->Head : Node->Link;
    dsunlock(&Collection->SyncObject);
    return Item;
}

OsStatus_t
CollectionRemoveByNode(
    _In_ Collection_t*     Collection,
    _In_ CollectionItem_t* Node)
{
    OsStatus_t Status = OsSuccess;
    
    assert(Collection != NULL);
    assert(Node != NULL);
    
    // Protect against double unlinking
    dslock(&Collection->SyncObject);
    if (Node->Link == NULL) {
        // Then the node should be the end of the list
        if (Collection->Tail != Node) {
            Status = OsDoesNotExist;
        }
    }
    else if (Node->Prev == NULL) {
        // Then the node should be the initial
        if (Collection->Head != Node) {
            Status = OsDoesNotExist;
        }
    }
    
    if (Status != OsDoesNotExist) {
        __collection_remove_node(Collection, Node);
        Node->Link = NULL;
        Node->Prev = NULL;
    }
    dsunlock(&Collection->SyncObject);
    return Status;
}

OsStatus_t
CollectionRemoveByKey(
    _In_ Collection_t* Collection, 
    _In_ DataKey_t     Key)
{
    CollectionItem_t* Node;
    OsStatus_t        Status;
    
    assert(Collection != NULL);

    Node = CollectionGetNodeByKey(Collection, Key, 0);
    if (Node != NULL) {
        Status = CollectionRemoveByNode(Collection, Node);
        if (Status != OsSuccess) {
            return Status;
        }
        return CollectionDestroyNode(Collection, Node);
    }
    return OsDoesNotExist;
}
