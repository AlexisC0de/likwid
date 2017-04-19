/*
 * =======================================================================================
 *
 *      Filename:  cpustring.c
 *
 *      Description:  Parser for CPU selection strings
 *
 *      Version:   <VERSION>
 *      Released:  <DATE>
 *
 *      Author:   Thomas Roehl (tr), thomas.roehl@googlemail.com
 *      Project:  likwid
 *
 *      Copyright (C) 2016 RRZE, University Erlangen-Nuremberg
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
#include <math.h>

#include <likwid.h>

/* #####   FUNCTION DEFINITIONS  -  LOCAL TO THIS SOURCE FILE   ########### */

static int
cpulist_sort(int* incpus, int* outcpus, int length)
{
    int insert = 0;
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    if (length <= 0)
    {
        return -1;
    }
    int inner_loop = ceil((double)length/cpuid_topology->numThreadsPerCore);
    for (int off = 0; off < cpuid_topology->numThreadsPerCore; off++)
    {
        for (int i = 0; i < inner_loop; i++)
        {
            outcpus[insert] = incpus[(i*cpuid_topology->numThreadsPerCore)+off];
            insert++;
        }
        if (insert == length)
            break;
    }
    return insert;
}

static int
cpulist_concat(int* cpulist, int startidx, int* addlist, int addlength)
{
    int count = 0;
    if (addlength <= 0)
    {
        return 0;
    }
    for (int i=startidx;i<(startidx+addlength);i++)
    {
        cpulist[i] = addlist[i-startidx];
        count++;
    }
    return count;
}

static int
cpu_in_domain(int domainidx, int cpu)
{
    affinity_init();
    AffinityDomains_t affinity = get_affinityDomains();
    for (int i=0;i<affinity->domains[domainidx].numberOfProcessors; i++)
    {
        if (cpu == affinity->domains[domainidx].processorList[i])
        {
            return 1;
        }
    }
    return 0;
}

static int
cpuexpr_to_list(bstring bcpustr, bstring prefix, int* list, int length)
{
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    affinity_init();
    AffinityDomains_t affinity = get_affinityDomains();
    struct bstrList* strlist;
    strlist = bsplit(bcpustr, ',');
    int oldinsert = 0;
    int insert = 0;
    for (int i=0;i < strlist->qty; i++)
    {
        bstring newstr = bstrcpy(prefix);
        bconcat(newstr, strlist->entry[i]);
        oldinsert = insert;
        for (int j = 0; j < affinity->numberOfAffinityDomains; j++)
        {
            if (bstrcmp(affinity->domains[j].tag, newstr) == 0)
            {
                list[insert] = atoi(bdata(strlist->entry[i]));
                insert++;
                if (insert == length)
                    goto list_done;
                break;
            }
        }
        if (insert == oldinsert)
        {
            fprintf(stderr,"Domain %s cannot be found\n", bdata(newstr));
        }
        bdestroy(newstr);
    }
list_done:
    bstrListDestroy(strlist);
    return insert;
}

static int
cpustr_to_cpulist_scatter(bstring bcpustr, int* cpulist, int length)
{
    int max_procs = 0;
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    affinity_init();
    AffinityDomains_t affinity = get_affinityDomains();
    char* cpustring = bstr2cstr(bcpustr, '\0');
    if (bstrchrp(bcpustr, ':', 0) != BSTR_ERR)
    {
        int insert = 0;
        int suitidx = 0;
        int* suitable = (int*)malloc(affinity->numberOfAffinityDomains*sizeof(int));
        if (!suitable)
        {
            bcstrfree(cpustring);
            return -ENOMEM;
        }
        for (int i=0; i<affinity->numberOfAffinityDomains; i++)
        {
            if (bstrchrp(affinity->domains[i].tag, cpustring[0], 0) != BSTR_ERR &&
                affinity->domains[i].numberOfProcessors > 0)
            {
                suitable[suitidx] = i;
                suitidx++;
                if (affinity->domains[i].numberOfProcessors > max_procs)
                    max_procs = affinity->domains[i].numberOfProcessors;
            }
        }
        int** sLists = (int**) malloc(suitidx * sizeof(int*));
        if (!sLists)
        {
            free(suitable);
            bcstrfree(cpustring);
            return -ENOMEM;
        }
        for (int i = 0; i< suitidx; i++)
        {
            sLists[i] = (int*) malloc(max_procs * sizeof(int));
            if (!sLists[i])
            {
                free(suitable);
                for (int j=0; i<i; j++)
                {
                    free(sLists[j]);
                }
                bcstrfree(cpustring);
                return -ENOMEM;
            }
            cpulist_sort(affinity->domains[suitable[i]].processorList, sLists[i], affinity->domains[suitable[i]].numberOfProcessors);
        }
        for (int off=0;off<max_procs;off++)
        {
            for(int i=0;i < suitidx; i++)
            {
                cpulist[insert] = sLists[i][off];
                insert++;
                if (insert == length)
                    goto scatter_done;
            }
        }
scatter_done:
        bcstrfree(cpustring);
        for (int i = 0; i< suitidx; i++)
        {
            free(sLists[i]);
        }
        free(sLists);
        free(suitable);
        return insert;
    }
    bcstrfree(cpustring);
    return 0;
}

static int
cpustr_to_cpulist_expression(bstring bcpustr, int* cpulist, int length)
{
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    affinity_init();
    AffinityDomains_t affinity = get_affinityDomains();
    bstring bdomain;
    int domainidx = -1;
    int count = 0;
    int stride = 0;
    int chunk = 0;
    if (bstrchrp(bcpustr, 'E', 0) != 0)
    {
        fprintf(stderr, "Not a valid CPU expression\n");
        return 0;
    }
    struct bstrList* strlist = bstrListCreate();
    strlist = bsplit(bcpustr, ':');
    if (strlist->qty == 3)
    {
        bdomain = bstrcpy(strlist->entry[1]);
        count = atoi(bdata(strlist->entry[2]));
        stride = 1;
        chunk = 1;
    }
    else if (strlist->qty == 5)
    {
        bdomain = bstrcpy(strlist->entry[1]);
        count = atoi(bdata(strlist->entry[2]));
        chunk = atoi(bdata(strlist->entry[3]));
        stride = atoi(bdata(strlist->entry[4]));
    }
    for (int i=0; i<affinity->numberOfAffinityDomains; i++)
    {
        if (bstrcmp(bdomain, affinity->domains[i].tag) == 0)
        {
            domainidx = i;
            break;
        }
    }
    if (domainidx < 0)
    {
        fprintf(stderr, "Cannot find domain %s\n", bdata(bdomain));
        bdestroy(bdomain);
        bstrListDestroy(strlist);
        return 0;
    }
    int offset = 0;
    int insert = 0;
    for (int i=0;i<count;i++)
    {
        for (int j=0; j<chunk && offset+j<affinity->domains[domainidx].numberOfProcessors;j++)
        {
            cpulist[insert] = affinity->domains[domainidx].processorList[offset + j];
            insert++;
            if (insert == length || insert == count)
                goto expression_done;
        }
        offset += stride;
        if (offset >= affinity->domains[domainidx].numberOfProcessors)
        {
            offset = 0;
        }
        if (insert >= count)
            goto expression_done;
    }
    bdestroy(bdomain);
    bstrListDestroy(strlist);
    return 0;
expression_done:
    bdestroy(bdomain);
    bstrListDestroy(strlist);
    return insert;
}

static int
cpustr_to_cpulist_logical(bstring bcpustr, int* cpulist, int length)
{
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    affinity_init();
    AffinityDomains_t affinity = get_affinityDomains();
    int domainidx = -1;
    bstring bdomain;
    bstring blist;
    struct bstrList* strlist;
    if (bstrchrp(bcpustr, 'L', 0) != 0)
    {
        fprintf(stderr, "ERROR: Not a valid CPU expression\n");
        return 0;
    }

    strlist = bsplit(bcpustr, ':');
    if (strlist->qty != 3)
    {
        fprintf(stderr, "ERROR: Invalid expression, should look like L:<domain>:<indexlist> or be in a cpuset\n");
        bstrListDestroy(strlist);
        return 0;
    }
    bdomain = bstrcpy(strlist->entry[1]);
    blist = bstrcpy(strlist->entry[2]);
    bstrListDestroy(strlist);
    for (int i=0; i<affinity->numberOfAffinityDomains; i++)
    {
        if (bstrcmp(bdomain, affinity->domains[i].tag) == 0)
        {
            domainidx = i;
            break;
        }
    }
    if (domainidx < 0)
    {
        fprintf(stderr, "ERROR: Cannot find domain %s\n", bdata(bdomain));
        bdestroy(bdomain);
        bdestroy(blist);
        return 0;
    }

    int *inlist = malloc(affinity->domains[domainidx].numberOfProcessors * sizeof(int));
    if (inlist == NULL)
    {
        bdestroy(bdomain);
        bdestroy(blist);
        return -ENOMEM;
    }

    int ret = cpulist_sort(affinity->domains[domainidx].processorList,
            inlist, affinity->domains[domainidx].numberOfProcessors);

    strlist = bsplit(blist, ',');
    int insert = 0;
    int insert_offset = 0;
    int inlist_offset = 0;
    int inlist_idx = 0;
    int require = 0;
    for (int i=0; i< strlist->qty; i++)
    {
        if (bstrchrp(strlist->entry[i], '-', 0) != BSTR_ERR)
        {
            struct bstrList* indexlist;
            indexlist = bsplit(strlist->entry[i], '-');
            if (atoi(bdata(indexlist->entry[0])) <= atoi(bdata(indexlist->entry[1])))
            {
                require += atoi(bdata(indexlist->entry[1])) - atoi(bdata(indexlist->entry[0])) + 1;
            }
            else
            {
                require += atoi(bdata(indexlist->entry[0])) - atoi(bdata(indexlist->entry[1])) + 1;
            }
        }
        else
        {
            require++;
        }
    }
    if (require > ret && getenv("LIKWID_SILENT") == NULL)
    {
        fprintf(stderr,
                "WARN: Selected affinity domain %s has only %d hardware threads, but selection string evaluates to %d threads.\n",
                bdata(affinity->domains[domainidx].tag), ret, require);
        fprintf(stderr, "      This results in multiple threads on the same hardware thread.\n");
    }
logical_redo:
    for (int i=0; i< strlist->qty; i++)
    {
        if (bstrchrp(strlist->entry[i], '-', 0) != BSTR_ERR)
        {
            struct bstrList* indexlist;
            indexlist = bsplit(strlist->entry[i], '-');
            if (atoi(bdata(indexlist->entry[0])) <= atoi(bdata(indexlist->entry[1])))
            {
                for (int j=atoi(bdata(indexlist->entry[0])); j<=atoi(bdata(indexlist->entry[1])) && (insert_offset+insert < require);j++)
                {
                    inlist_idx = j;
                    cpulist[insert_offset + insert] = inlist[inlist_idx % ret];
                    insert++;
                    if (insert == ret)
                    {
                        bstrListDestroy(indexlist);
                        if (insert == require)
                            goto logical_done;
                        else
                            goto logical_redo;
                    }
                }
            }
            else
            {
                for (int j=atoi(bdata(indexlist->entry[0]));
                        j>=atoi(bdata(indexlist->entry[1])) && (insert_offset+insert < require); j--)
                {
                    inlist_idx = j;
                    cpulist[insert_offset + insert] = inlist[inlist_idx % ret];
                    insert++;
                    if (insert == ret)
                    {
                        bstrListDestroy(indexlist);
                        if (insert == require)
                            goto logical_done;
                        else
                            goto logical_redo;
                    }
                }
            }
            bstrListDestroy(indexlist);
        }
        else
        {
            cpulist[insert_offset + insert] = inlist[atoi(bdata(strlist->entry[i])) % ret];
            insert++;
            if (insert == ret)
            {
                if (insert == require)
                    goto logical_done;
                else
                    goto logical_redo;
            }
        }
    }
logical_done:
    bdestroy(bdomain);
    bdestroy(blist);
    bstrListDestroy(strlist);
    free(inlist);
    return require;
}

static int
cpustr_to_cpulist_physical(bstring bcpustr, int* cpulist, int length)
{
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    affinity_init();
    AffinityDomains_t affinity = get_affinityDomains();
    bstring bdomain;
    bstring blist;
    int domainidx = -1;
    struct bstrList* strlist;
    if (bstrchrp(bcpustr, ':', 0) != BSTR_ERR)
    {
        strlist = bsplit(bcpustr, ':');
        bdomain = bstrcpy(strlist->entry[0]);
        blist = bstrcpy(strlist->entry[1]);
        bstrListDestroy(strlist);
    }
    else
    {
        bdomain = bformat("N");
        blist = bstrcpy(bcpustr);
    }
    for (int i=0; i<affinity->numberOfAffinityDomains; i++)
    {
        if (bstrcmp(bdomain, affinity->domains[i].tag) == 0)
        {
            domainidx = i;
            break;
        }
    }
    if (domainidx < 0)
    {
        fprintf(stderr, "Cannot find domain %s\n", bdata(bdomain));
        bdestroy(bdomain);
        bdestroy(blist);
        return 0;
    }

    strlist = bsplit(blist, ',');
    int insert = 0;
    for (int i=0;i< strlist->qty; i++)
    {
        if (bstrchrp(strlist->entry[i], '-', 0) != BSTR_ERR)
        {
            struct bstrList* indexlist;
            indexlist = bsplit(strlist->entry[i], '-');
            if (atoi(bdata(indexlist->entry[0])) <= atoi(bdata(indexlist->entry[1])))
            {
                for (int j=atoi(bdata(indexlist->entry[0])); j<=atoi(bdata(indexlist->entry[1]));j++)
                {
                    if (cpu_in_domain(domainidx, j))
                    {
                        cpulist[insert] = j;
                        insert++;
                        if (insert == length)
                        {
                            bstrListDestroy(indexlist);
                            goto physical_done;
                        }
                    }
                    else
                    {
                        fprintf(stderr, "CPU %d not in domain %s\n", j, bdata(affinity->domains[domainidx].tag));
                    }
                }
            }
            else
            {
                for (int j=atoi(bdata(indexlist->entry[0])); j>=atoi(bdata(indexlist->entry[1]));j--)
                {
                    if (cpu_in_domain(domainidx, j))
                    {
                        cpulist[insert] = j;
                        insert++;
                        if (insert == length)
                        {
                            bstrListDestroy(indexlist);
                            goto physical_done;
                        }
                    }
                    else
                    {
                        fprintf(stderr, "CPU %d not in domain %s\n", j, bdata(affinity->domains[domainidx].tag));
                    }
                }
            }
            bstrListDestroy(indexlist);
        }
        else
        {
            int cpu = atoi(bdata(strlist->entry[i]));
            if (cpu_in_domain(domainidx, cpu))
            {
                cpulist[insert] = cpu;
                insert++;
                if (insert == length)
                {
                    goto physical_done;
                }
            }
            else
            {
                fprintf(stderr, "CPU %d not in domain %s\n", cpu, bdata(affinity->domains[domainidx].tag));
            }
        }
    }
physical_done:
    bstrListDestroy(strlist);
    bdestroy(bdomain);
    bdestroy(blist);
    return insert;
}

/* #####   FUNCTION DEFINITIONS  -  EXPORTED FUNCTIONS   ################## */

int
cpustr_to_cpulist(const char* cpustring, int* cpulist, int length)
{
    int insert = 0;
    int len = 0;
    int ret = 0;
    bstring bcpustr = bfromcstr(cpustring);
    struct bstrList* strlist;
    bstring scattercheck = bformat("scatter");
    topology_init();
    CpuTopology_t cpuid_topology = get_cpuTopology();
    strlist = bsplit(bcpustr, '@');

    int* tmpList = (int*)malloc(length * sizeof(int));
    if (tmpList == NULL)
    {
        bstrListDestroy(strlist);
        bdestroy(scattercheck);
        bdestroy(bcpustr);
        return -ENOMEM;
    }
    for (int i=0; i< strlist->qty; i++)
    {
        if (binstr(strlist->entry[i], 0, scattercheck) != BSTR_ERR)
        {
            ret = cpustr_to_cpulist_scatter(strlist->entry[i], tmpList, length);
            insert += cpulist_concat(cpulist, insert, tmpList, ret);
        }
        else if (bstrchrp(strlist->entry[i], 'E', 0) == 0)
        {
            ret = cpustr_to_cpulist_expression(strlist->entry[i], tmpList, length);
            insert += cpulist_concat(cpulist, insert, tmpList, ret);
        }
        else if (bstrchrp(strlist->entry[i], 'L', 0) == 0)
        {
            ret = cpustr_to_cpulist_logical(strlist->entry[i], tmpList, length);
            insert += cpulist_concat(cpulist, insert, tmpList, ret);
        }
        else if (cpuid_topology->activeHWThreads < cpuid_topology->numHWThreads)
        {
            fprintf(stdout,
                    "INFO: You are running LIKWID in a cpuset with %d CPUs, only logical numbering allowed\n",
                    cpuid_topology->activeHWThreads);
            if (((bstrchrp(strlist->entry[i], 'N', 0) == 0) ||
                (bstrchrp(strlist->entry[i], 'S', 0) == 0) ||
                (bstrchrp(strlist->entry[i], 'C', 0) == 0) ||
                (bstrchrp(strlist->entry[i], 'M', 0) == 0)) &&
                (bstrchrp(strlist->entry[i], ':', 0) != BSTR_ERR))
            {
                bstring newstr = bformat("L:");
                bconcat(newstr, strlist->entry[i]);
                ret = cpustr_to_cpulist_logical(newstr, tmpList, length);
                insert += cpulist_concat(cpulist, insert, tmpList, ret);
                bdestroy(newstr);
            }
            else
            {
                bstring newstr = bformat("L:N:");
                bconcat(newstr, strlist->entry[i]);
                ret = cpustr_to_cpulist_logical(newstr, tmpList, length);
                insert += cpulist_concat(cpulist, insert, tmpList, ret);
                bdestroy(newstr);
            }
        }
        else if (((bstrchrp(strlist->entry[i], 'N', 0) == 0) ||
            (bstrchrp(strlist->entry[i], 'S', 0) == 0) ||
            (bstrchrp(strlist->entry[i], 'C', 0) == 0) ||
            (bstrchrp(strlist->entry[i], 'M', 0) == 0)) &&
            (bstrchrp(strlist->entry[i], ':', 0) != BSTR_ERR))
        {
            bstring newstr = bformat("L:");
            bconcat(newstr, strlist->entry[i]);
            ret = cpustr_to_cpulist_logical(newstr, tmpList, length);
            insert += cpulist_concat(cpulist, insert, tmpList, ret);
            bdestroy(newstr);
        }

        else
        {
            ret = cpustr_to_cpulist_physical(strlist->entry[i], tmpList, length);
            insert += cpulist_concat(cpulist, insert, tmpList, ret);
        }
    }
    free(tmpList);
    bdestroy(bcpustr);
    bdestroy(scattercheck);
    bstrListDestroy(strlist);
    return insert;
}

int
nodestr_to_nodelist(const char* nodestr, int* nodes, int length)
{
    int ret = 0;
    bstring prefix = bformat("M");
    bstring bnodestr = bfromcstr(nodestr);
    ret = cpuexpr_to_list(bnodestr, prefix, nodes, length);
    bdestroy(bnodestr);
    bdestroy(prefix);
    return ret;
}

int
sockstr_to_socklist(const char* sockstr, int* sockets, int length)
{
    int ret = 0;
    bstring prefix = bformat("S");
    bstring bsockstr = bfromcstr(sockstr);
    ret = cpuexpr_to_list(bsockstr, prefix, sockets, length);
    bdestroy(bsockstr);
    bdestroy(prefix);
    return ret;
}

