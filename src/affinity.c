/*
 * =======================================================================================
 *
 *      Filename:  affinity.c
 *
 *      Description:  Implementation of affinity module.
 *
 *      Version:   4.3.2
 *      Released:  12.04.2018
 *
 *      Author:   Jan Treibig (jt), jan.treibig@gmail.com,
 *                Thomas Roehl (tr), thomas.roehl@googlemail.com
 *      Project:  likwid
 *
 *      Copyright (C) 2018 RRZE, University Erlangen-Nuremberg
 *
 *      This program is free software: you can redistribute it and/or modify it under
 *      the terms of the GNU General Public License as published by the Free Software
 *      Foundation, either version 3 of the License, or (at your option) any later
 *      version.
 *
 *      This program is distributed in the hope that it will be useful, but WITHOUT ANY
 *      WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 *      PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along with
 *      this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * =======================================================================================
 */

/* #####   HEADER FILE INCLUDES   ######################################### */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

#include <types.h>
#include <error.h>
#include <likwid.h>
#include <numa.h>
#include <affinity.h>
#include <lock.h>
#include <tree.h>
#include <topology.h>
#include <topology_hwloc.h>

/* #####   EXPORTED VARIABLES   ########################################### */

int *affinity_thread2core_lookup = NULL;
int *affinity_thread2socket_lookup = NULL;
int *affinity_thread2numa_lookup = NULL;
int *affinity_thread2sharedl3_lookup = NULL;

/* #####   MACROS  -  LOCAL TO THIS SOURCE FILE   ######################### */

#define gettid() syscall(SYS_gettid)

/* #####   VARIABLES  -  LOCAL TO THIS SOURCE FILE   ###################### */

static int  affinity_numberOfDomains = 0;
static AffinityDomain*  domains;
static int affinity_initialized = 0;

AffinityDomains affinityDomains;

/* #####   FUNCTION DEFINITIONS  -  LOCAL TO THIS SOURCE FILE   ########### */

static int
getProcessorID(cpu_set_t* cpu_set)
{
    int processorId;
    topology_init();

    for ( processorId = 0; processorId < cpuid_topology.numHWThreads; processorId++ )
    {
        if ( CPU_ISSET(processorId,cpu_set) )
        {
            break;
        }
    }
    return processorId;
}

static int
treeFillNextEntries(
    TreeNode* tree,
    int* processorIds,
    int startidx,
    int socketId,
    int coreOffset,
    int coreSpan,
    int numberOfEntries)
{
    int counter = numberOfEntries;
    int skip = 0;
    int c, t, c_count = 0;
    TreeNode* node = tree;
    TreeNode* thread;
    node = tree_getChildNode(node);

    /* get socket node */
    for (int i=0; i<socketId; i++)
    {
        node = tree_getNextNode(node);
        if ( node == NULL )
        {
            DEBUG_PRINT(DEBUGLEV_DEVELOP, Cannot find socket %d in topology tree, i);
        }
    }

    node = tree_getChildNode(node);
    /* skip offset cores */
    for (int i=0; i<coreOffset; i++)
    {
        node = tree_getNextNode(node);

        if ( node == NULL )
        {
            DEBUG_PRINT(DEBUGLEV_DEVELOP, Cannot find core %d in topology tree, i);
        }
    }

    /* Traverse horizontal */
    while ( node != NULL && c_count < coreSpan)
    {
        if ( !counter ) break;

        thread = tree_getChildNode(node);

        while ( thread != NULL && (numberOfEntries-counter) < numberOfEntries )
        {
            if (cpuid_topology.threadPool[thread->id].inCpuSet)
            {
                processorIds[startidx+(numberOfEntries-counter)] = thread->id;
                thread = tree_getNextNode(thread);
                counter--;
            }
            else
            {
                thread = tree_getNextNode(thread);
            }
        }
        c_count++;
        node = tree_getNextNode(node);
    }
    return numberOfEntries-counter;
}

static int get_id_of_type(hwloc_obj_t base, hwloc_obj_type_t type)
{
    hwloc_obj_t walker = base->parent;
    while (walker && walker->type != type)
        walker = walker->parent;
    if (walker && walker->type == type)
        return walker->os_index;
    return -1;
}

static int create_lookups()
{
    topology_init();
    if (!affinity_thread2core_lookup)
    {
        affinity_thread2core_lookup = malloc(cpuid_topology.numHWThreads * sizeof(int));
        memset(affinity_thread2core_lookup, -1, cpuid_topology.numHWThreads*sizeof(int));
    }
    if (!affinity_thread2socket_lookup)
    {
        affinity_thread2socket_lookup = malloc(cpuid_topology.numHWThreads * sizeof(int));
        memset(affinity_thread2socket_lookup, -1, cpuid_topology.numHWThreads*sizeof(int));
    }
    if (!affinity_thread2sharedl3_lookup)
    {
        affinity_thread2sharedl3_lookup = malloc(cpuid_topology.numHWThreads * sizeof(int));
        memset(affinity_thread2sharedl3_lookup, -1, cpuid_topology.numHWThreads*sizeof(int));
    }
    if (!affinity_thread2numa_lookup)
    {
        affinity_thread2numa_lookup = malloc(cpuid_topology.numHWThreads * sizeof(int));
        memset(affinity_thread2numa_lookup, -1, cpuid_topology.numHWThreads*sizeof(int));
    }

    int num_pu = likwid_hwloc_get_nbobjs_by_type(hwloc_topology, HWLOC_OBJ_PU);
    for (int pu_idx = 0; pu_idx < num_pu; pu_idx++)
    {
        hwloc_obj_t pu = likwid_hwloc_get_obj_by_type(hwloc_topology, HWLOC_OBJ_PU, pu_idx);
        int hwthreadid = pu->os_index;
        int coreid = get_id_of_type(pu, HWLOC_OBJ_CORE);
        int sockid = get_id_of_type(pu, HWLOC_OBJ_PACKAGE);
        int memid = get_id_of_type(pu, HWLOC_OBJ_NUMANODE);
        affinity_thread2core_lookup[hwthreadid] = coreid;
        DEBUG_PRINT(DEBUGLEV_DEVELOP, affinity_thread2core_lookup[%d] = %d, hwthreadid, coreid);
        affinity_thread2socket_lookup[hwthreadid] = sockid;
        DEBUG_PRINT(DEBUGLEV_DEVELOP, affinity_thread2socket_lookup[%d] = %d, hwthreadid, sockid);
        affinity_thread2numa_lookup[hwthreadid] = memid;
        DEBUG_PRINT(DEBUGLEV_DEVELOP, affinity_thread2numa_lookup[%d] = %d, hwthreadid, memid);
    }
    int maxNumLevels = 0;
    int depth = likwid_hwloc_topology_get_depth(hwloc_topology);
    for (int d = 1; d <= depth; d++)
    {
        if (likwid_hwloc_get_depth_type(hwloc_topology, d) == HWLOC_OBJ_CACHE)
            maxNumLevels++;
    }
    for(int d=depth-1;d >= 1; d--)
    {
        if (likwid_hwloc_get_depth_type(hwloc_topology, d) == HWLOC_OBJ_CACHE)
        {
            hwloc_obj_t cobj = likwid_hwloc_get_obj_by_depth(hwloc_topology, d, 0);
            if (cobj->attr->cache.depth != 3)
            {
                continue;
            }
            int num_caches = likwid_hwloc_get_nbobjs_by_depth(hwloc_topology, d);
            for (int c = 0; c < num_caches; c++)
            {
                cobj = likwid_hwloc_get_obj_by_depth(hwloc_topology, d, c);
                for (int i = 0; i < num_pu; i++)
                {
                    if (likwid_hwloc_bitmap_isset(cobj->cpuset, i))
                    {
                        affinity_thread2sharedl3_lookup[i] = c;
                        DEBUG_PRINT(DEBUGLEV_DEVELOP, affinity_thread2sharedl3_lookup[%d] = %d, i, c);
                    }
                }
            }
        }
    }
    return 0;
}

/*static int create_locks()*/
/*{*/
/*    numa_init();*/
/*    if (!socket_lock)*/
/*    {*/
/*        socket_lock = malloc(cpuid_topology.numSockets * sizeof(int));*/
/*        memset(socket_lock, LOCK_INIT, cpuid_topology.numSockets*sizeof(int));*/
/*    }*/
/*    if (!tile_lock)*/
/*    {*/
/*        tile_lock = malloc(cpuid_topology.numHWThreads * sizeof(int));*/
/*        memset(tile_lock, LOCK_INIT, cpuid_topology.numHWThreads*sizeof(int));*/
/*    }*/
/*    if (!numa_lock)*/
/*    {*/
/*        numa_lock = malloc(numa_info.numberOfNodes * sizeof(int));*/
/*        memset(numa_lock, LOCK_INIT, numa_info.numberOfNodes*sizeof(int));*/
/*    }*/
/*    if (!core_lock)*/
/*    {*/
/*        int cores = (cpuid_topology.numHWThreads/cpuid_topology.numThreadsPerCore);*/
/*        core_lock = malloc(cores * sizeof(int));*/
/*        memset(core_lock, LOCK_INIT, cores*sizeof(int));*/
/*    }*/
/*    if (!sharedl2_lock)*/
/*    {*/
/*        sharedl2_lock = malloc(cpuid_topology.numHWThreads * sizeof(int));*/
/*        memset(sharedl2_lock, LOCK_INIT, cpuid_topology.numHWThreads*sizeof(int));*/
/*    }*/
/*    if (!sharedl3_lock)*/
/*    {*/
/*        sharedl3_lock = malloc(cpuid_topology.numHWThreads * sizeof(int));*/
/*        memset(sharedl3_lock, LOCK_INIT, cpuid_topology.numHWThreads*sizeof(int));*/
/*    }*/
/*}*/

/* #####   FUNCTION DEFINITIONS  -  EXPORTED FUNCTIONS   ################## */

void
affinity_init()
{
    int numberOfDomains = 1; /* all systems have the node domain */
    int currentDomain;
    int subCounter = 0;
    int offset = 0;
    int tmp;
    if (affinity_initialized == 1)
    {
        return;
    }
    topology_init();

    create_lookups();
    //create_locks();
    int numberOfSocketDomains = cpuid_topology.numSockets;

    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: Socket domains %d, numberOfSocketDomains);
    numa_init();
    int numberOfNumaDomains = numa_info.numberOfNodes;
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: NUMA domains %d, numberOfNumaDomains);
    int numberOfProcessorsPerSocket =
        cpuid_topology.numCoresPerSocket * cpuid_topology.numThreadsPerCore;
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: CPUs per socket %d, numberOfProcessorsPerSocket);
    int numberOfCacheDomains;

    int numberOfCoresPerCache =
        cpuid_topology.cacheLevels[cpuid_topology.numCacheLevels-1].threads/
        cpuid_topology.numThreadsPerCore;
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: CPU cores per LLC %d, numberOfCoresPerCache);

    int numberOfProcessorsPerCache =
        cpuid_topology.cacheLevels[cpuid_topology.numCacheLevels-1].threads;
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: CPUs per LLC %d, numberOfProcessorsPerCache);
    /* for the cache domain take only into account last level cache and assume
     * all sockets to be uniform. */

    /* determine how many last level shared caches exist per socket */
    numberOfCacheDomains = cpuid_topology.numSockets *
        (cpuid_topology.numCoresPerSocket/numberOfCoresPerCache);
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: Cache domains %d, numberOfCacheDomains);
    /* determine total number of domains */
    numberOfDomains += numberOfSocketDomains + numberOfCacheDomains + numberOfNumaDomains;
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity: All domains %d, numberOfDomains);

    domains = (AffinityDomain*) malloc(numberOfDomains * sizeof(AffinityDomain));
    if (!domains)
    {
        fprintf(stderr,"No more memory for %ld bytes for array of affinity domains\n",numberOfDomains * sizeof(AffinityDomain));
        return;
    }


    /* Node domain */
    domains[0].numberOfProcessors = cpuid_topology.activeHWThreads;
    domains[0].numberOfCores = cpuid_topology.numSockets * cpuid_topology.numCoresPerSocket;
    DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity domain N: %d HW threads on %d cores, domains[0].numberOfProcessors, domains[0].numberOfCores);
    domains[0].tag = bformat("N");
    domains[0].processorList = (int*) malloc(cpuid_topology.numHWThreads*sizeof(int));
    if (!domains[0].processorList)
    {
        fprintf(stderr,"No more memory for %ld bytes for processor list of affinity domain %s\n",
                cpuid_topology.numHWThreads*sizeof(int),
                bdata(domains[0].tag));
        return;
    }
    offset = 0;
    if (numberOfSocketDomains > 1)
    {
        for (int i=0; i<numberOfSocketDomains; i++)
        {
          tmp = treeFillNextEntries(cpuid_topology.topologyTree,
                                    domains[0].processorList, offset,
                                    i, 0,
                                    cpuid_topology.numCoresPerSocket, numberOfProcessorsPerSocket);
          offset += tmp;
        }
    }
    else
    {
        tmp = treeFillNextEntries(cpuid_topology.topologyTree,
                                  domains[0].processorList, 0,
                                  0, 0,
                                  domains[0].numberOfCores, domains[0].numberOfProcessors);
        domains[0].numberOfProcessors = tmp;
    }

    /* Socket domains */
    currentDomain = 1;
    tmp = 0;
    for (int i=0; i < numberOfSocketDomains; i++ )
    {
        domains[currentDomain + i].numberOfProcessors = numberOfProcessorsPerSocket;
        domains[currentDomain + i].numberOfCores =  cpuid_topology.numCoresPerSocket;
        domains[currentDomain + i].tag = bformat("S%d", i);
        DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity domain S%d: %d HW threads on %d cores, i, domains[currentDomain + i].numberOfProcessors, domains[currentDomain + i].numberOfCores);
        domains[currentDomain + i].processorList = (int*) malloc( domains[currentDomain + i].numberOfProcessors * sizeof(int));
        memset(domains[currentDomain + i].processorList, 0, domains[currentDomain + i].numberOfProcessors * sizeof(int));
        if (!domains[currentDomain + i].processorList)
        {
            fprintf(stderr,"No more memory for %ld bytes for processor list of affinity domain %s\n",
                    domains[currentDomain + i].numberOfProcessors * sizeof(int),
                    bdata(domains[currentDomain + i].tag));
            return;
        }
        tmp = treeFillNextEntries(cpuid_topology.topologyTree,
                                  domains[currentDomain + i].processorList, 0,
                                  i, 0, cpuid_topology.numCoresPerSocket,
                                  domains[currentDomain + i].numberOfProcessors);
        tmp = MIN(tmp, domains[currentDomain + i].numberOfProcessors);
        domains[currentDomain + i].numberOfProcessors = tmp;
    }

    /* Cache domains */
    currentDomain += numberOfSocketDomains;
    subCounter = 0;
    for (int i=0; i < numberOfSocketDomains; i++ )
    {
        offset = 0;

        for ( int j=0; j < (numberOfCacheDomains/numberOfSocketDomains); j++ )
        {
            domains[currentDomain + subCounter].numberOfProcessors = numberOfProcessorsPerCache;
            domains[currentDomain + subCounter].numberOfCores =  numberOfCoresPerCache;
            domains[currentDomain + subCounter].tag = bformat("C%d", subCounter);
            DEBUG_PRINT(DEBUGLEV_DEVELOP, Affinity domain C%d: %d HW threads on %d cores, subCounter, domains[currentDomain + subCounter].numberOfProcessors, domains[currentDomain + subCounter].numberOfCores);
            domains[currentDomain + subCounter].processorList = (int*) malloc(numberOfProcessorsPerCache*sizeof(int));
            if (!domains[currentDomain + subCounter].processorList)
            {
                fprintf(stderr,"No more memory for %ld bytes for processor list of affinity domain %s\n",
                        numberOfProcessorsPerCache*sizeof(int),
                        bdata(domains[currentDomain + subCounter].tag));
                return;
            }
            tmp = treeFillNextEntries(cpuid_topology.topologyTree,
                                      domains[currentDomain + subCounter].processorList, 0,
                                      i, offset, numberOfCoresPerCache,
                                      domains[currentDomain + subCounter].numberOfProcessors);

            domains[currentDomain + subCounter].numberOfProcessors = tmp;
            offset += (tmp < numberOfCoresPerCache ? tmp : numberOfCoresPerCache);
            subCounter++;
        }
    }
    /* Memory domains */
    currentDomain += numberOfCacheDomains;
    subCounter = 0;
    if ((numberOfNumaDomains >= numberOfSocketDomains) && (numberOfNumaDomains > 1))
    {
        for (int i=0; i < numberOfSocketDomains; i++ )
        {
            offset = 0;
            for ( int j=0; j < (int)ceil((double)(numberOfNumaDomains)/numberOfSocketDomains); j++ )
            {
                domains[currentDomain + subCounter].numberOfProcessors =
                                numa_info.nodes[subCounter].numberOfProcessors;

                domains[currentDomain + subCounter].numberOfCores =
                                numa_info.nodes[subCounter].numberOfProcessors/cpuid_topology.numThreadsPerCore;

                domains[currentDomain + subCounter].tag = bformat("M%d", subCounter);

                DEBUG_PRINT(DEBUGLEV_DEVELOP,
                        Affinity domain M%d: %d HW threads on %d cores,
                        subCounter, domains[currentDomain + subCounter].numberOfProcessors,
                        domains[currentDomain + subCounter].numberOfCores);

                domains[currentDomain + subCounter].processorList =
                                (int*) malloc(numa_info.nodes[subCounter].numberOfProcessors*sizeof(int));

                if (!domains[currentDomain + subCounter].processorList)
                {
                    fprintf(stderr,"No more memory for %ld bytes for processor list of affinity domain %s\n",
                            numa_info.nodes[subCounter].numberOfProcessors*sizeof(int),
                            bdata(domains[currentDomain + subCounter].tag));
                    return;
                }
                if (offset >= cpuid_topology.numCoresPerSocket*cpuid_topology.numSockets)
                {
                    continue;
                }
                tmp = treeFillNextEntries(cpuid_topology.topologyTree,
                                          domains[currentDomain + subCounter].processorList, 0,
                                          i, offset, domains[currentDomain + subCounter].numberOfCores,
                                          domains[currentDomain + subCounter].numberOfProcessors);
                domains[currentDomain + subCounter].numberOfProcessors = tmp;
                offset += domains[currentDomain + subCounter].numberOfCores;
                subCounter++;
            }
        }
    }
    else
    {
        offset = 0;
        int NUMAthreads = numberOfProcessorsPerSocket * numberOfSocketDomains;
        domains[currentDomain + subCounter].numberOfProcessors = NUMAthreads;
        domains[currentDomain + subCounter].numberOfCores =  NUMAthreads/cpuid_topology.numThreadsPerCore;
        domains[currentDomain + subCounter].tag = bformat("M%d", subCounter);

        DEBUG_PRINT(DEBUGLEV_DEVELOP,
                Affinity domain M%d: %d HW threads on %d cores,
                subCounter, domains[currentDomain + subCounter].numberOfProcessors,
                domains[currentDomain + subCounter].numberOfCores);

        domains[currentDomain + subCounter].processorList = (int*) malloc(NUMAthreads*sizeof(int));
        if (!domains[currentDomain + subCounter].processorList)
        {
            fprintf(stderr,"No more memory for %ld bytes for processor list of affinity domain %s\n",
                    NUMAthreads*sizeof(int),
                    bdata(domains[currentDomain + subCounter].tag));
            return;
        }
        tmp = 0;
        for (int i=0; i < numberOfSocketDomains; i++ )
        {
            tmp += treeFillNextEntries(
                cpuid_topology.topologyTree,
                domains[currentDomain + subCounter].processorList, tmp,
                i, 0, domains[currentDomain + subCounter].numberOfCores,
                numberOfProcessorsPerSocket);
            offset += numberOfProcessorsPerSocket;
        }
        domains[currentDomain + subCounter].numberOfProcessors = tmp;
    }

    affinity_numberOfDomains = numberOfDomains;
    affinityDomains.numberOfAffinityDomains = numberOfDomains;
    affinityDomains.numberOfSocketDomains = numberOfSocketDomains;
    affinityDomains.numberOfNumaDomains = numberOfNumaDomains;
    affinityDomains.numberOfProcessorsPerSocket = numberOfProcessorsPerSocket;
    affinityDomains.numberOfCacheDomains = numberOfCacheDomains;
    affinityDomains.numberOfCoresPerCache = numberOfCoresPerCache;
    affinityDomains.numberOfProcessorsPerCache = numberOfProcessorsPerCache;
    affinityDomains.domains = domains;
    affinity_initialized = 1;
}

void
affinity_finalize()
{
    if (affinity_initialized == 0)
    {
        return;
    }
    if (!affinityDomains.domains)
    {
        return;
    }
    for ( int i=0; i < affinityDomains.numberOfAffinityDomains; i++ )
    {
        bdestroy(affinityDomains.domains[i].tag);
        if (affinityDomains.domains[i].processorList != NULL)
        {
            free(affinityDomains.domains[i].processorList);
        }
        affinityDomains.domains[i].processorList = NULL;
    }
    if (affinityDomains.domains != NULL)
    {
        free(affinityDomains.domains);
        affinityDomains.domains = NULL;
    }
    if (affinity_thread2core_lookup)
    {
        free(affinity_thread2core_lookup);
        affinity_thread2core_lookup = NULL;
    }
    if (affinity_thread2socket_lookup)
    {
        free(affinity_thread2socket_lookup);
        affinity_thread2socket_lookup = NULL;
    }
    if (affinity_thread2sharedl3_lookup)
    {
        free(affinity_thread2sharedl3_lookup);
        affinity_thread2sharedl3_lookup = NULL;
    }
    if (affinity_thread2numa_lookup)
    {
        free(affinity_thread2numa_lookup);
        affinity_thread2numa_lookup = NULL;
    }
/*    if (socket_lock)*/
/*        free(socket_lock);*/
/*    if (core_lock)*/
/*        free(core_lock);*/
/*    if (tile_lock)*/
/*        free(tile_lock);*/
/*    if (numa_lock)*/
/*        free(numa_lock);*/
/*    if (sharedl2_lock)*/
/*        free(sharedl2_lock);*/
/*    if (sharedl3_lock)*/
/*        free(sharedl3_lock);*/

    affinityDomains.domains = NULL;
    affinity_numberOfDomains = 0;
    affinityDomains.numberOfAffinityDomains = 0;
    affinityDomains.numberOfSocketDomains = 0;
    affinityDomains.numberOfNumaDomains = 0;
    affinityDomains.numberOfProcessorsPerSocket = 0;
    affinityDomains.numberOfCacheDomains = 0;
    affinityDomains.numberOfCoresPerCache = 0;
    affinityDomains.numberOfProcessorsPerCache = 0;
    affinity_initialized = 0;
}

int
affinity_processGetProcessorId()
{
    int ret;
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    ret = sched_getaffinity(getpid(),sizeof(cpu_set_t), &cpu_set);

    if (ret < 0)
    {
        ERROR;
    }

    return getProcessorID(&cpu_set);
}

int
affinity_threadGetProcessorId()
{
    cpu_set_t  cpu_set;
    CPU_ZERO(&cpu_set);
    sched_getaffinity(gettid(),sizeof(cpu_set_t), &cpu_set);

    return getProcessorID(&cpu_set);
}

#ifdef HAS_SCHEDAFFINITY
void
affinity_pinThread(int processorId)
{
    cpu_set_t cpuset;
    pthread_t thread;

    thread = pthread_self();
    CPU_ZERO(&cpuset);
    CPU_SET(processorId, &cpuset);
    pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
}
#else
void
affinity_pinThread(int processorId)
{
}
#endif

void
affinity_pinProcess(int processorId)
{
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(processorId, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

void
affinity_pinProcesses(int cpu_count, const int* processorIds)
{
    int i;
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    for(i=0;i<cpu_count;i++)
    {
        CPU_SET(processorIds[i], &cpuset);
    }
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
}

const AffinityDomain*
affinity_getDomain(bstring domain)
{

    for ( int i=0; i < affinity_numberOfDomains; i++ )
    {
        if ( biseq(domain, domains[i].tag) )
        {
            return domains+i;
        }
    }

    return NULL;
}

void
affinity_printDomains()
{
    for ( int i=0; i < affinity_numberOfDomains; i++ )
    {
        printf("Domain %d:\n",i);
        printf("\tTag %s:",bdata(domains[i].tag));

        for ( uint32_t j=0; j < domains[i].numberOfProcessors; j++ )
        {
            printf(" %d",domains[i].processorList[j]);
        }
        printf("\n");
    }
}

AffinityDomains_t
get_affinityDomains(void)
{
    return &affinityDomains;
}

