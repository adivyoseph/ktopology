/* 
 kubelet tolopgy modeling
 
 builds a topolgy map
    include l3group memory (numaMem/L3GroupCnt)
    SMT can be enabled
    SMT protection can be enabled, affects shared vCpu's also
 reserves cores for system deamons first
 buils init shared core pool
 K8 API
    POD_requests
        POD hash
        vCPU ask, whole number < 16 maps to exclusive allocation
                  whole number > 25 maps to a pool of ask/100 cores and shared scheduling
        memory ask, whole number in GBytes
    POD release
        POD hash
 
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>

/*
    system topology information
*/
typedef struct system_s {
    int numaCnt;           //number of numa instances, default 1
    int l3groupPerNuma;
    int coresPerL3group;
    int smtOn;              //set to 1 to enable SMT
    int smtProtect;         //set 1 to prevent SMT security risks
    int memoryPerL3group;   //number of GBytes, allocations are in whole number multiples
    int memBandwidthPerL3group;     //not currently used
    int reservedCores;
    int reservedCoreMemory;
    int sharedPoolDefaultCoreCnt;
    int sharedPoolDefaultMemory;
    
} system_t;

system_t g_systemInfo;

/*
    resource manager info
*/

typedef struct core_s {
    int l3GroupId;
    int systemCoreId;
    int systemL3groupId;
    int systemNumaId;
    int sibling;
    //
    int state;
#define CORE_IDLE   0
#define CORE_EXCLUSIVE  2
#define CORE_SHARED     3
#define CORE_RESERVED  1
    //exclusive
    int podOwnerId;
    //shared
    int sharedPoolId;
} core_t;


typedef struct l3Group_s {
    int numaL3groupId;
    int systemL3groupId;
    int coreCnt;
    int memory;
    core_t cores[64];

    //
    int memoryInuse;
    int coresIdle;
    int coresReserved;
    int coresShared;
    int coresExclusive;
    int Shared[64];

} l3Group_t;

typedef struct numa_s {
    int numaId;
    int coreCnt;
    int l3groupCnt;         //per numa
    l3Group_t l3Groups[8];

    int freeL3Groups;
    int freeCores;



} numa_t;

typedef struct resources_s {
    int numaCnt;
    int l3groupCnt;         //system wide
    int coreCnt;            //system wide
    numa_t numas[4];


    //
    int nextNuma;

} resources_t;

resources_t resources;

typedef struct pod_s {
    int index;
    int state;
#define POD_ALLOCATED 1
#define POD_RESERVED  2
    int type;               //needed for release
    char podId[16];
    int memory;
    //for exclusive only
    int coreCnt;
    int cores[32];
    //for shared only
    int coreCredits;
    int sharedpoolInd;

} pod_t;

typedef struct sharedPool_s {
    int index;
    int state;

    int coreCnt;
    int podRefs;
    int pods[32];
    int cores[16];
    int l3GroupId;
    int memory;
    int memoryInuse;
    int coresInuse;             //100 credits equals one core



}  sharedPool_t;


sharedPool_t sharedPools[512];
pod_t pods[256];
core_t *p_cores[512];
l3Group_t *p_l3Groups[64];

int systemInfo_init(void);
int pod_init(void);
int sharedPool_init(void);
int sharedPool_new(int, int);
sharedPool_t *  sharedPool_find(int, int);
int sharedPool_releasePod(pod_t *p_pod);
pod_t * pod_findFree(void);
int resourceManager_init(void);
int print_topology(void);
int k8_api(char        *p_line);
int rm_applyReserved(void);
int rm_buildExclusiveList(int count, int memory, int *p_list);

/**
 * @author martin (11/9/22)
 * @brief start
 * @param argc 
 * @param argv 
 * 
 * @return int never used
 */
int main(int argc, char **argv) {
    FILE        *p_f;
    char        *p_line = NULL;
    size_t      len = 0;
    ssize_t     read;


    systemInfo_init();
    resourceManager_init();
    pod_init();
    sharedPool_init();
    rm_applyReserved();
    sharedPool_new(g_systemInfo.sharedPoolDefaultCoreCnt, g_systemInfo.sharedPoolDefaultMemory);
    if (g_systemInfo.numaCnt > 1) {
        sharedPool_new(g_systemInfo.sharedPoolDefaultCoreCnt, g_systemInfo.sharedPoolDefaultMemory);
    }
    p_f = fopen("../test", "r");
    if (p_f == NULL)
        exit(EXIT_FAILURE);

    while ((read = getline(&p_line, &len, p_f)) != -1) {
        //printf("Retrieved line of length %zu:\n", read);
        //printf("%s", p_line);
        k8_api(p_line);
    }

    print_topology();
    fclose(p_f);
    if (p_line)
        free(p_line);

    return 0;
}

int pod_init(void){
    int i;

    for (i = 0; i < 256; i++) {
        pods[i].index = i;
        pods[i].state = 0;
    }
    return 0;
}

pod_t * pod_findFree(void){
    pod_t *p_pod = NULL;
    int i;

    for (i = 0; i < 256; i++) {
        if (pods[i].state == 0) {
            pods[i].state = POD_ALLOCATED; //reserved
            p_pod = &pods[i];
            printf("pod_findFree %d\n", i);
            break;
        }
    }
    return p_pod;
}


int sharedPool_init(void){
    sharedPool_t *p_pool = NULL;
    int i, j;

    for (i = 0; i < 512; i++) {
        p_pool = &sharedPools[i];

        p_pool->state = 0;
        p_pool->index = i;
        for (j = 0; j < 32; j++) {
            p_pool->pods[j] = -1;
        }


    }


    return 0;
}

sharedPool_t *sharedPool_findFree(void){
    sharedPool_t *p_pool = NULL;
    int i;

    for (i = 0; i < 512; i++) {
        if (sharedPools[i].state == 0) {
            p_pool = &sharedPools[i];
            p_pool->state = 1;
            break;
        }
    }
    return p_pool;
}

/**
 * @brief 
 * 
 * @author martin (11/12/22)
 * 
 * @param coresAsk 
 * @param memoryAsk 
 * 
 * @return int 
 */
int sharedPool_new(int coreAsk, int memoryAsk){
    int   coresResp[32];
    sharedPool_t *p_pool = NULL;
    l3Group_t   *p_l3g = NULL;
    core_t *p_core;
    p_pool = sharedPool_findFree();
    int i, rc = 0;

    if (rm_buildExclusiveList(coreAsk, memoryAsk, &coresResp[0])){
        //got a list
        p_pool->coreCnt = coreAsk;
        p_pool->memory  = memoryAsk;
        p_pool->podRefs = 0;
        p_pool->coresInuse   = 0;
        p_pool->memoryInuse  = 0;
        p_pool->l3GroupId    = p_cores[coresResp[0]]->systemL3groupId;
        p_l3g = p_l3Groups[p_pool->l3GroupId];
        p_l3g->coresIdle -= coreAsk;
        p_l3g->coresShared += coreAsk;
        p_l3g->memoryInuse += memoryAsk;
        for (i = 0; i < coreAsk; i++) {
            p_core = p_cores[coresResp[i]];
            p_core->state = CORE_SHARED;
            p_core->sharedPoolId = p_pool->index;
            p_pool->cores[i] = p_core->systemCoreId;

        }
        rc = 1;
    }
    else {
        p_pool->state = 0;
    }

    return rc;
}

/**
 * @brief
 * 
 * @author martin (11/12/22)
 * 
 * @param coreAsk   100 credits equals one core
 * @param memoryAsk 
 * 
 * @return int 
 */
sharedPool_t * sharedPool_find(int coreAsk, int memoryAsk){
    sharedPool_t *p_pool = NULL;
    sharedPool_t *p_rc = NULL;
//    core_t *p_core;
    int i, work;;

    for (i = 0; i < 512; i++) {
        if (sharedPools[i].state) {
            p_pool = &sharedPools[i];
            if (memoryAsk <= (p_pool->memory - p_pool->memoryInuse)) {
                printf("\tpool %d enough memory\n", i);
                work = p_pool->coreCnt * 100;
                if (coreAsk <= (work - p_pool->coresInuse)) {
                    printf("\tpool %d enough core credits\n", i);
                    p_rc = p_pool;
                    break;
                }
                else {
                    printf("\tpool %d not enough core credits\n", i);
                }
            }
            else {
                printf("\tpool %d not enough memory\n", i);
            }
        }
    }
    return p_rc;
}


int sharedPool_podAttach(sharedPool_t *p_pool, pod_t *p_pod, int coreAsk, int memoryAsk){
    int i;
    p_pod->type = CORE_SHARED;
    p_pod->sharedpoolInd = p_pool->index;
    p_pod->coreCnt = coreAsk;       //for cleanup
    p_pod->memory = memoryAsk;
    p_pool->podRefs++;
    p_pool->coresInuse   += coreAsk;
    p_pool->memoryInuse  += memoryAsk;
    //p_pool->pods[] entries can come and go at any time
    for (i = 0; i < 32; i++ ) {
        if (p_pool->pods[i] < 0) {
            p_pool->pods[i] = p_pod->index;
            break;
        }
    }


    return 0;
}

int sharedPool_releasePod(pod_t *p_pod){
    sharedPool_t *p_pool = NULL;
    int i;

    p_pool = &sharedPools[p_pod->sharedpoolInd];
    //clean up pool first
    for (i = 0; i < 32; i++ ) {
        if (p_pool->pods[i] == p_pod->index) {
            p_pool->pods[i] = -1;
            break;
        }
    }
    p_pool->podRefs--;
    p_pool->coresInuse -= p_pod->coreCnt;
    p_pool->memoryInuse -= p_pod->memory;
    //clean up pod
    p_pod->state = 0;
    p_pod->type = -1;




    return 0;
}


int rm_reserveList(int newState, pod_t *p_pod, int *p_list, int count, int memory){
    int i;
    int rc = 0;
    core_t *p_core;
    int *p_coreList = p_list;

    l3Group_t   *p_l3g = NULL;

    p_pod->coreCnt = count;


    p_core = p_cores[*p_coreList];
    p_l3g = p_l3Groups[p_core->systemL3groupId];
    p_l3g->memoryInuse += memory;
    p_pod->memory = memory;

    for (i = 0; i < count; i++, p_coreList++) {
        p_core = p_cores[*p_coreList];
        switch (newState) {
            case CORE_RESERVED:
                p_l3g->coresIdle --;
                p_l3g->coresReserved++;
                break;
            case CORE_EXCLUSIVE:
                p_l3g->coresIdle --;
                p_l3g->coresExclusive++;
                break;
            default:
                break;
         }
         p_core->state = newState;
         p_pod->type = newState;
         p_core->podOwnerId = p_pod->index;  //Pod instance
         p_pod->cores[i] = p_core->systemCoreId;
    }
    return rc;
}


int rm_buildExclusiveList(int count, int memory, int *p_list){

    int *p_work = p_list;
    int         i,j,k,l, m ;
    numa_t      *p_numa = NULL;
    l3Group_t   *p_l3G = NULL;
    core_t      *p_core = NULL;
    int done = 0;

    m = 0;
    p_numa = &resources.numas[resources.nextNuma];
    printf("ExclusiveList  count %d   memory %d\n", count, memory);
    for (i = 0; i < resources.numaCnt; i++) {
        printf("\tstart on numa %d\n", p_numa->numaId);
        for (j = 0; j < g_systemInfo.l3groupPerNuma; j++) {
            p_l3G = &p_numa->l3Groups[j];
            //first check for memory
            printf("\tl3 %d free mem %d ask %d\n", j, (p_l3G->memory - p_l3G->memoryInuse), memory);
            if (memory > (p_l3G->memory - p_l3G->memoryInuse)) {
                continue;
            }
            //now we start looking
            if (count <= p_l3G->coresIdle ) {
                //lets try
                for (k = 0; k < p_l3G->coreCnt; k++) {
                    p_core = &p_l3G->cores[k];
                    if (p_core->state == CORE_IDLE) {
                        //walk to the end
                        printf("\tfound idle numa=%d l3=%d core=%d\n",
                               p_numa->numaId, j, k);
                        for (l = k, m = 0; l < p_l3G->coreCnt; l++, m++) {
                            if(p_l3G->cores[k].state != CORE_IDLE) {
                                break;
                            }
                        }
                        printf("\tm = %d\n", m);
                        if (m >= count) {
                            //found a sequence of cores in idle state
                            for (l = k, m = 0; m < count; l++, m++, p_work++) {
                                *p_work = p_l3G->cores[l].systemCoreId;
                            }
                            printf("\tdone\n");
                            done = 1;
                            break;
                        }

                    }
                }

            }
            if (done) {
                break;
            }
        }

        resources.nextNuma++;
        if (resources.nextNuma >= resources.numaCnt) {
            resources.nextNuma = 0;
        }
        p_numa = &resources.numas[resources.nextNuma];
        if (done) {
            break;
        }
    }



    return done;
}



int rm_applyReserved(void){
    /*
    always take from the first l3Group 
    g_systemInfo.reservedCores count to reserve 
    */
    char *p_podId = "9999999";
    int   coresResp[32];
    pod_t *p_pod;
    int i;
    int reserveAsk = g_systemInfo.reservedCores;
    if(g_systemInfo.smtProtect & g_systemInfo.smtOn){
        //round up to create a gaurd
        if (reserveAsk & 0x1) {
            reserveAsk++;
        }
    }

    p_pod = pod_findFree();
    if (p_pod != NULL) {
        memcpy(p_pod->podId, p_podId, 8);
        printf("REG: reserved cores %d mem %d\n", reserveAsk, g_systemInfo.reservedCoreMemory);

        if(rm_buildExclusiveList(reserveAsk, g_systemInfo.reservedCoreMemory, &coresResp[0])){
            //got a list
            for (i = 0; i < reserveAsk; i++) {
                printf("\trm_applyReserved %d %d\n", i, coresResp[i]);
            }
            rm_reserveList(CORE_RESERVED, p_pod, &coresResp[0], reserveAsk, g_systemInfo.reservedCoreMemory);
        }
    }
 
    return 0;
}

void print_str(char *pr){
    char *p = pr;
    do {
        printf("%02x ", *p);
        p++;

    } while (*p != 0);
    printf("\n");
}


pod_t *pod_find(char *p_id){
    pod_t *p_pod = NULL;
    pod_t *p_rc = NULL;
    int i, j;

    for (i = 0; i < 256; i++) {
        p_pod = &pods[i];
        if (p_pod->state == 0) {
            break;
        }
        for (j = 0; j < 8; j++) {
            if (p_pod->podId[j] != p_id[j]) {
                break;
            }
        }
        if (j ==8) {
            p_rc = p_pod;
            break;
        }
    }
    return p_rc;
}

int rm_releaseExclusive( pod_t *p_pod){
    int i;
    int rc = 0;
    core_t *p_core;
    l3Group_t   *p_l3g = NULL;

    printf("releaseExclusive: POD %d  cores %d\n", p_pod->index, p_pod->coreCnt);

    p_core = p_cores[p_pod->cores[0]];
    p_l3g = p_l3Groups[p_core->systemL3groupId];
    //release memory
    p_l3g->memoryInuse -= p_pod->memory;
    p_pod->memory = 0;
    //release cores

    for (i = 0; i < p_pod->coreCnt; i++) {
        p_core = p_cores[p_pod->cores[i]];
        p_l3g->coresIdle ++;
        p_l3g->coresExclusive--;
        p_core->state = CORE_IDLE;
        p_core->podOwnerId = -1;  //Pod instance
        p_pod->cores[i] = -1;
        printf("\tcore %d\n", p_core->systemCoreId);
    }
    p_pod->state = 0;
    return rc;
}

int k8_api(char        *p){
    int coreAsk;
    int coreMaxPerL3 = g_systemInfo.coresPerL3group;
    int memoryAsk;
//    int typeAsk;
    char        *p_line = p;
    int  i, j;
    char *p_token[8];

    int   coresResp[32];
    pod_t *p_pod;
    sharedPool_t *p_pool = NULL;

    //print_str(p_line);
    j =  0;
    do {
        p_token[j] = p_line;
        //print_str(p_token[j]);
        j++;
        do {
            p_line++;
            //print_str(p_line);
        } while ((*p_line != ' ') && (*p_line != '\n'));
        if (*p_line == '\n') {
            *p_line = 0;
            break;
        }
        *p_line = 0;
        p_line++;
    } while(1);
/*
    for (i = 0; i < j; i++) {
        printf("token_%d %s\n", i, p_token[i]);
    }
*/
    if (!strcmp(p_token[0], "req")) {
        coreAsk = atoi(p_token[2]);
        memoryAsk = atoi(p_token[3]);
        if (coreAsk >24) {
            //shared
            printf("REQ: cores %d share\n", coreAsk);
            if (coreAsk > 100) {
                printf("  more than 1 core\n");
            }
            else {
                printf("  less than 1 core\n");
            }
            //todo add support for shared pools

            p_pool = sharedPool_find(coreAsk, memoryAsk);
            if (p_pool) {
                //found a fit
                p_pod = pod_findFree();
                if (p_pod != NULL) {
                    //attach POD
                    memcpy(p_pod->podId, p_token[1], 8);

                    sharedPool_podAttach(p_pool, p_pod, coreAsk, memoryAsk);
                }
                else{
                    printf("\tran out of POD trackers\n");
                }
            }
            else {
                printf("\tneed new pool\n");
                if(sharedPool_new(coreAsk *4, memoryAsk*4)){
                    p_pool = sharedPool_find(coreAsk, memoryAsk);
                    if (p_pool) {
                        //found a fit
                        p_pod = pod_findFree();
                        if (p_pod != NULL) {
                            //attach POD
                            memcpy(p_pod->podId, p_token[1], 8);

                            sharedPool_podAttach(p_pool, p_pod, coreAsk, memoryAsk);
                        }
                    }
                }
            }

        }
        else {
            if (g_systemInfo.smtOn) {
                coreMaxPerL3 += g_systemInfo.coresPerL3group;
            }
            if (coreAsk <= coreMaxPerL3) {
                //eclusive 
                //check memory
                if (memoryAsk < g_systemInfo.memoryPerL3group){

                    if(g_systemInfo.smtProtect && g_systemInfo.smtOn && (coreAsk & 0x1)){
                        //round up to create a gaurd
                        coreAsk++;
                        printf("REG: exclusive coresPadded %d mem %d\n", coreAsk , memoryAsk);
                    }
                    else {
                        printf("REG: exclusive cores %d mem %d\n", coreAsk , memoryAsk);
                    }

                    //ok we have a chance
                    p_pod = pod_findFree();
                    if (p_pod != NULL) {

                        memcpy(p_pod->podId, p_token[1], 8);

                        if(rm_buildExclusiveList(coreAsk, memoryAsk, &coresResp[0])){
                            //got a list
                            for (i = 0; i < coreAsk; i++) {
                                printf("\trm_applyReserved %d %d\n", i, coresResp[i]);
                            }
                            rm_reserveList(CORE_EXCLUSIVE, p_pod, &coresResp[0], coreAsk, memoryAsk);
                        }
                        else{
                            printf("\trejected\n");
                            p_pod->state = 0;
                        }

                    }
                    else {
                        printf("REQ: cores %d memAsk %d\n", coreAsk, memoryAsk);
                        printf("  rejected could not allocate a POD tracker \n");
                    }
                }
                else {
                    printf("REQ: cores %d memAsk %d\n", coreAsk, memoryAsk);
                    printf("  rejected memoryAsk %d > %d memoryPerL3group \n", memoryAsk, g_systemInfo.memoryPerL3group);
                }
            }
            else {
                printf("REQ: cores %d\n", coreAsk);
                printf("  rejected coreAsk %d > %d coreMaxPerL3\n", coreAsk, coreMaxPerL3);
            }
        }
    }
    else if (!strcmp(p_token[0], "rel")) {
        printf("REL:\n");
        p_pod = pod_find(p_token[1]);
        if (p_pod != NULL) {
            //
            printf("\tPOD found type %d\n", p_pod->type);
            switch (p_pod->type) {
            case CORE_EXCLUSIVE:
                rm_releaseExclusive(p_pod);
                break;
            case CORE_SHARED:
                sharedPool_releasePod(p_pod);
                break;

            default:
                break;
            }



        }
        else {
            printf("\trejected POD not found\n");
        }

    }



 
    return 0;
}

int resourceManager_init(void){
    int         i, j, k;
    numa_t      *p_numa = NULL;
    l3Group_t   *p_l3G = NULL;
    core_t      *p_core = NULL;


    resources.nextNuma = 0;

    resources.coreCnt = 0;
    resources.l3groupCnt = 0;
    resources.numaCnt = g_systemInfo.numaCnt;
    for (i = 0; i < resources.numaCnt; i++) {
        p_numa = &resources.numas[i];
        p_numa->numaId = i;
        p_numa->coreCnt = 0;
        p_numa->l3groupCnt = 0;

        for (j = 0; j < g_systemInfo.l3groupPerNuma; j++) {
            p_l3G = &p_numa->l3Groups[j];
            p_l3G->numaL3groupId = j;      
            p_l3G->systemL3groupId = resources.l3groupCnt;      //system wide
            p_l3Groups[resources.l3groupCnt] = &p_numa->l3Groups[j];
            p_l3G->coreCnt = 0;
            p_l3G->memory = g_systemInfo.memoryPerL3group;
            resources.l3groupCnt++;      
            p_numa->l3groupCnt++;      
                             
            for (k = 0; k < (g_systemInfo.coresPerL3group * 2); ) {   //a core may have SMT on, so my be 2 SW visible cores
                p_core = &p_l3G->cores[k];
                p_core->l3GroupId = k;
                p_core->systemCoreId = resources.coreCnt;
                p_core->systemL3groupId = p_l3G->systemL3groupId;
                p_core->systemNumaId = p_numa->numaId;
                p_core->state = 0;
                p_cores[resources.coreCnt] = &p_l3G->cores[k];
                p_l3G->coreCnt++;
                p_numa->coreCnt++;
                resources.coreCnt++;
                if (g_systemInfo.smtOn) {
                    if (p_core->l3GroupId & 0x1) {
                        //odd
                        p_core->sibling = p_core->l3GroupId -1;
                    }
                    else {
                        //even
                        p_core->sibling = p_core->l3GroupId +1;
                    }
                    k++;
                }
                else{
                    p_core->sibling = -1;
                    k++;
                    if (k == g_systemInfo.coresPerL3group) {
                        break;
                    }
                }
                
            }
            p_l3G->memoryInuse = 0;
            p_l3G->coresIdle = p_l3G->coreCnt;
            p_l3G->coresReserved = 0;;
            p_l3G->coresShared = 0;
            p_l3G->coresExclusive = 0;
        }
        p_numa->freeL3Groups = p_numa->l3groupCnt;
        p_numa->freeCores = p_numa->coreCnt;
    }

    return 0;
}

int systemInfo_init(void){
    g_systemInfo.numaCnt = 2;
    g_systemInfo.l3groupPerNuma = 4;
    g_systemInfo.coresPerL3group = 8;
    g_systemInfo.smtOn = 1;
    g_systemInfo.smtProtect = 0;
    g_systemInfo.reservedCores = 2;
    g_systemInfo.reservedCoreMemory = 8;
    g_systemInfo.memoryPerL3group = 64; 

    g_systemInfo.sharedPoolDefaultCoreCnt = 4;
    g_systemInfo.sharedPoolDefaultMemory = 16;



    return 0;
}


int print_topology(void){
    int         i, j, k;
    numa_t      *p_numa = NULL;
    l3Group_t   *p_l3G = NULL;
    core_t      *p_core = NULL;
    pod_t       *p_pod = NULL;
    sharedPool_t *p_pool = NULL;

    printf("System: Nuna=%d  l3Groups=%d cores=%d\n",
                    resources.numaCnt,
                    resources.l3groupCnt,
                    resources.coreCnt
           );

    for (i = 0; i < resources.numaCnt; i++) {
        p_numa = &resources.numas[i];
        printf("\tNuma_%d: %d/%d \n",
                        p_numa->numaId,
                        p_numa->l3groupCnt,
                        p_numa->coreCnt
        );
        for (j = 0; j < g_systemInfo.l3groupPerNuma; j++) {
            p_l3G = &p_numa->l3Groups[j];
            printf("\t\tl3Group_%d: %d, memInUse=%d idle=%d res=%d\n",
                   j, 
                   g_systemInfo.l3groupPerNuma,
                   p_l3G->memoryInuse,
                   p_l3G->coresIdle,
                   p_l3G->coresReserved
                   );
            for (k = 0; k < p_l3G->coreCnt; k++){
                p_core = &p_l3G->cores[k];
                switch (p_core->state) {
                case CORE_IDLE:
                    printf("\t\t\tcore_%d: %d  sibling %d  state IDLE \n",
                           p_core->systemCoreId,
                           p_core->l3GroupId,
                           p_core->sibling
                           );
                    break;
                case CORE_RESERVED:
                     printf("\t\t\tcore_%d: %d  sibling %d  state RESERVED\n",
                           p_core->systemCoreId,
                           p_core->l3GroupId,
                           p_core->sibling
                            );
                     break;
                case CORE_EXCLUSIVE:
                    printf("\t\t\tcore_%d: %d  sibling %d  state EXCLUSIVE  pod %d\n",
                           p_core->systemCoreId,
                           p_core->l3GroupId,
                           p_core->sibling,
                           p_core->podOwnerId
                           );
                    break;

                case CORE_SHARED:
                    printf("\t\t\tcore_%d: %d  sibling %d  state SHARED  sharedPool %d\n",
                           p_core->systemCoreId,
                           p_core->l3GroupId,
                           p_core->sibling,
                           p_core->sharedPoolId
                           );
                    break;
                default:
                    break;
                }

            }

        }
    }
    printf("\n\n");
    for (i = 0; i < 256; i++) {
        p_pod = &pods[i];
        if (p_pod->state == 0) {
            continue;
        }
        printf("POD_%d state=%d type=%d memory=%d\n", i, p_pod->state, p_pod->type, p_pod->memory);
        if (p_pod->type == CORE_EXCLUSIVE) {       
            for (j = 0; j < p_pod->coreCnt; j++) {
            printf("\t%d  core %d:%d:%d, state %d\n",
                    j, 
                   p_pod->cores[j], 
                   p_cores[p_pod->cores[j]]->systemL3groupId,
                   p_cores[p_pod->cores[j]]->systemNumaId,
                   p_cores[p_pod->cores[j]]->state);
            }
        }
        else {
            //assume shared
            printf("\tsharedPool %d \n", p_pod->sharedpoolInd);

        }
 
    }
    printf("\n\n");
    for (i = 0; i < 512; i++) {
        p_pool = &sharedPools[i];
        if (p_pool->state) {
            printf("POOL_%d \n", i);
            for (j= 0; j < p_pool->coreCnt; j++) {
                printf("\t%d core %d\n", j, p_pool->cores[j]);
            }
            printf("\t--  podRefs %d\n", p_pool->podRefs);
            for (j = 0; j < 32; j++) {
                if (p_pool->pods[j] >= 0) {
                    printf("\t%d POD %d\n", j, p_pool->pods[j]);
                }
            }
            printf("\tcores= %d used= %d   memory= %d  used= %d\n",
                   p_pool->coreCnt*100,
                   p_pool->coresInuse,
                   p_pool->memory,
                   p_pool->memoryInuse
                   );



        }
    }






    return 0;
}
