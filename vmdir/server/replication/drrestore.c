/*
 * Copyright © 2018 VMware, Inc.  All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the “License”); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS, without
 * warranties or conditions of any kind, EITHER EXPRESS OR IMPLIED.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */



/*
 * Module Name: Replication
 *
 * Filename: drrestore.c
 *
 * Abstract: DR node restore from backup DB
 *
 */

#include "includes.h"
#define DR_DC_LIST "lw-dr-dc-list.txt"

static
DWORD
_VmDirDeleteDCObject(
    PDEQUE  pDequeDCObjUPN
    );

static
DWORD
_VmDirDeleteServerObjectTree(
    PDEQUE  pDequeDCObjUPN
    );

static
DWORD
_VmDirDumpQ(
    PDEQUE  pDequeDCObjUPN
    );

static
DWORD
_VmDirDeleteManagedService(
    PDEQUE  pDequeDCObjUPN
    );

static
DWORD
_VmDirDeriveDRNodeUtdVector(
    PSTR*   ppszUtdVector
    );

/* This function re-instantiates the current vmdir instance with a
 * foreign (MDB) database file.
 *
 * 1. foreign DB should be copy to /var/lib/vmware/vmdir/partner directory
 * 2. re-initialize backend with foreign DB (this include schema and index reload)
 * 3. derive UTDVector of DR node from foreign DB
 * 4. clean up foreign DB
 *    a) delete domain controllers
 *    b) delete server object tree
 *    c) delete managed service object tree
 *    d) delete DNS zone tree
 * 5. advance server id, so step 6 will pick up next available server id
 * 6. create server object of DR node (also update vmwMaxServerId to system domain entry)
 * 7. patch server object to have proper UTDVector
 * 8. create RA container
 * 9. patch DSE Root entry
 */
DWORD
VmDirSrvServerReset(
    PDWORD pServerResetState
    )
{
    DWORD dwError = 0;
    const char  *dbHomeDir = VMDIR_DB_DIR;
    PVDIR_SCHEMA_CTX    pSchemaCtx = NULL;
    BOOLEAN             bWriteInvocationId = FALSE;
    DEQUE               deqDCObjUPN = {0};
    PVMDIR_DR_DC_INFO   pDcInfo = NULL;
    PSTR                pszUtdVector = NULL;
    BOOLEAN             bMdbWalEnable = FALSE;
    VDIR_BERVALUE       bvUtdVector = VDIR_BERVALUE_INIT;

    VmDirGetMdbWalEnable(&bMdbWalEnable);

    VmDirBkgdThreadShutdown();

    VmDirMetricsShutdown();

    //swap current vmdir database file with the foriegn one under partner/
    dwError = VmDirSwapDB(dbHomeDir, bMdbWalEnable);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Swap DB", __FUNCTION__);

    VmDirSetACLMode();

    // from backup db, derive proper utdvector for DR node.
    dwError = _VmDirDeriveDRNodeUtdVector(&pszUtdVector);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = _VmDirDeleteDCObject(&deqDCObjUPN);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Delete DC objects", __FUNCTION__);

    dwError = _VmDirDeleteServerObjectTree(&deqDCObjUPN);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Delete Directory Server object tree", __FUNCTION__);

    dwError = _VmDirDeleteManagedService(&deqDCObjUPN);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Delete Managed Service objects", __FUNCTION__);

    dwError = _VmDirDumpQ(&deqDCObjUPN);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Dumped queue to file", __FUNCTION__);

    // shutdown and init ACL to pick up domain sid from restored DB
    VmDirVmAclShutdown();

    dwError = VmDirVmAclInit();
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Reinitialize ACL subsystem", __FUNCTION__);

    dwError = VmDirSchemaCtxAcquire(&pSchemaCtx );
    BAIL_ON_VMDIR_ERROR(dwError);

    // pick up maxServerId from restored DB to advance gVmdirServerGlobals.serverId
    dwError = VmDirSetGlobalServerId();
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Advance server id", __FUNCTION__);

    dwError = VmDirSrvCreateServerObj(pSchemaCtx);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Create Directory Server object", __FUNCTION__);

    // set utdvector of server object
    bvUtdVector.lberbv_val = pszUtdVector;
    bvUtdVector.lberbv_len = VmDirStringLenA(pszUtdVector);
    dwError = VmDirInternalEntryAttributeReplace(
        pSchemaCtx,
        gVmdirServerGlobals.serverObjDN.lberbv.bv_val,
        ATTR_UP_TO_DATE_VECTOR,
        &bvUtdVector);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSrvCreateReplAgrsContainer(pSchemaCtx);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirPatchDSERoot(pSchemaCtx);
    BAIL_ON_VMDIR_ERROR(dwError);
    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s Updaet DSE Root", __FUNCTION__);

    VmDirSchemaCtxRelease(pSchemaCtx);
    pSchemaCtx = NULL;

    dwError = LoadServerGlobals(&bWriteInvocationId);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirMetricsInitialize();
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirBkgdThreadInitialize();
    BAIL_ON_VMDIR_ERROR(dwError);

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszUtdVector);
    VmDirSchemaCtxRelease(pSchemaCtx);

    while(!dequeIsEmpty(&deqDCObjUPN))
    {
        dequePopLeft(&deqDCObjUPN, (PVOID*)&pDcInfo);
        VMDIR_SAFE_FREE_MEMORY(pDcInfo->pszSite);
        VMDIR_SAFE_FREE_MEMORY(pDcInfo->pszUPN);
        VMDIR_SAFE_FREE_MEMORY(pDcInfo);
    }
    return dwError;

error:
    goto cleanup;
}

static
DWORD
_VmDirDeriveDRNodeUtdVector(
    PSTR*   ppszUtdVector
    )
{
    DWORD   dwError = 0;
    PSTR    pszFQDN = NULL;
    PSTR    pszLocalUtdVector = NULL;
    PSTR    pszLocalMaxUSN = NULL;
    PVDIR_ENTRY     pEntry = NULL;
    PVDIR_ATTRIBUTE pAttrServerDN = NULL;

    dwError = VmDirSimpleDNToEntry(PERSISTED_DSE_ROOT_DN, &pEntry);
    BAIL_ON_VMDIR_ERROR(dwError);

    pAttrServerDN = VmDirEntryFindAttribute(ATTR_SERVER_NAME, pEntry);
    assert(pAttrServerDN);

    VMDIR_LOG_INFO( VMDIR_LOG_MASK_ALL, "%s backup DB was taken from %s", __FUNCTION__, pAttrServerDN->vals[0].lberbv_val);

    dwError = VmDirDnLastRDNToCn(pAttrServerDN->vals[0].lberbv_val, &pszFQDN);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirComposeNodeUtdVector(pszFQDN, &pszLocalUtdVector, &pszLocalMaxUSN);
    BAIL_ON_VMDIR_ERROR(dwError);

    *ppszUtdVector = pszLocalUtdVector;
    pszLocalUtdVector = NULL;

cleanup:
    VMDIR_SAFE_FREE_MEMORY(pszFQDN);
    VMDIR_SAFE_FREE_MEMORY(pszLocalUtdVector);
    VMDIR_SAFE_FREE_MEMORY(pszLocalMaxUSN);
    VmDirFreeEntry(pEntry);

    return dwError;

error:
    goto cleanup;
}

// remove all DC under ou=domain controllers,SYSTEM_DOMAIN
static
DWORD
_VmDirDeleteDCObject(
    PDEQUE  pDequeDCObjUPN
    )
{
    DWORD   dwError = 0;
    int     i = 0;
    PSTR    pszDomainControllerContainerDn = NULL;
    PSTR    pszDCObj = NULL;
    PVDIR_ATTRIBUTE pAttrUPN = NULL;
    VDIR_ENTRY_ARRAY entryArray = {0};
    PVMDIR_DR_DC_INFO pQueueStruct = NULL;

    //Delete domain controllers
    dwError = VmDirAllocateStringPrintf(&
        pszDomainControllerContainerDn,
        "ou=%s,%s",
        VMDIR_DOMAIN_CONTROLLERS_RDN_VAL,
        gVmdirServerGlobals.systemDomainDN.lberbv_val);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSimpleEqualFilterInternalSearch(
        pszDomainControllerContainerDn,
        LDAP_SCOPE_ONE,
        ATTR_OBJECT_CLASS,
        OC_COMPUTER,
        &entryArray);
    BAIL_ON_VMDIR_ERROR(dwError);

    if(entryArray.iSize > 0)
    {
        for (i = 0; i < entryArray.iSize; i++)
        {
            pAttrUPN = VmDirFindAttrByName(&entryArray.pEntry[i], ATTR_KRB_UPN);
            if (pAttrUPN)
            {
                VmDirAllocateMemory(sizeof(VMDIR_DR_DC_INFO), (PVOID*)&pQueueStruct);
                BAIL_ON_VMDIR_ERROR(dwError);

                dwError = VmDirAllocateStringA(pAttrUPN->vals[0].lberbv_val, &pszDCObj);
                BAIL_ON_VMDIR_ERROR(dwError);

                pQueueStruct->pszUPN = pszDCObj;

                dwError = dequePush(pDequeDCObjUPN, pQueueStruct);
                BAIL_ON_VMDIR_ERROR(dwError);
                pszDCObj = NULL;
                pQueueStruct = NULL;
            }

            dwError = VmDirDeleteEntry(&entryArray.pEntry[i]);
            BAIL_ON_VMDIR_ERROR(dwError);
        }
    }

cleanup:
    VmDirFreeEntryArrayContent(&entryArray);
    VMDIR_SAFE_FREE_MEMORY(pszDomainControllerContainerDn);
    VMDIR_SAFE_FREE_MEMORY(pszDCObj);
    VMDIR_SAFE_FREE_MEMORY(pQueueStruct);

    return dwError;

error:
    goto cleanup;
}

// remove all DC object tree cn=Configuration,SYSTEM_DOMAIN
static
DWORD
_VmDirDeleteServerObjectTree(
    PDEQUE  pDequeDCObjUPN
    )
{
    DWORD   dwError = 0;
    int     i = 0;
    PSTR    pszConfigurationContainerDn = NULL;
    VDIR_ENTRY_ARRAY entryArray = {0};
    PVMDIR_DR_DC_INFO pQueueStruct = NULL;
    PSTR    pszSite = NULL;
    PSTR    pszCN = NULL;
    PDEQUE_NODE pDequeTmp = NULL;

    dwError = VmDirAllocateStringPrintf(
        &pszConfigurationContainerDn,
        "cn=%s,%s",
        VMDIR_CONFIGURATION_CONTAINER_NAME,
        gVmdirServerGlobals.systemDomainDN.lberbv_val);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSimpleEqualFilterInternalSearch(
        pszConfigurationContainerDn,
        LDAP_SCOPE_SUBTREE,
        ATTR_OBJECT_CLASS,
        OC_DIR_SERVER,
        &entryArray);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (entryArray.iSize > 0)
    {
        for (i = 0; i < entryArray.iSize; i++)
        {
            /* Delete all replication agreement entries for a server and
             * the server it self under the configuration/site container
             */
            dwError = VmDirServerDNToSite(entryArray.pEntry[i].dn.lberbv_val, &pszSite);
            BAIL_ON_VMDIR_ERROR(dwError);

            for (pDequeTmp = pDequeDCObjUPN->pHead; pDequeTmp != NULL; pDequeTmp = pDequeTmp->pNext)
            {
                pQueueStruct = (PVMDIR_DR_DC_INFO) pDequeTmp->pElement;
                VmDirDnLastRDNToCn(entryArray.pEntry[i].dn.lberbv_val, &pszCN);
                BAIL_ON_VMDIR_ERROR(dwError);

                if (VmDirStringLenA(pszCN) >= VmDirStringLenA(pQueueStruct->pszUPN) ||
                    VmDirStringNCompareA(pszCN, pQueueStruct->pszUPN, VmDirStringLenA(pszCN), FALSE) ||
                    pQueueStruct->pszUPN[VmDirStringLenA(pszCN)] != '@')
                {
                    VMDIR_SAFE_FREE_MEMORY(pszCN);
                    continue;
                }

                pQueueStruct->pszSite = pszSite;
                pszSite = NULL;
                VMDIR_SAFE_FREE_MEMORY(pszCN);
                break;
            }

            dwError = VmDirInternalDeleteTree(entryArray.pEntry[i].dn.lberbv_val, TRUE);
            BAIL_ON_VMDIR_ERROR(dwError);
        }
    }

cleanup:
    VmDirFreeEntryArrayContent(&entryArray);
    VMDIR_SAFE_FREE_MEMORY(pszConfigurationContainerDn);
    VMDIR_SAFE_FREE_MEMORY(pszSite);

    return dwError;
error:
    goto cleanup;
}

// remove all DNS objects under dc=DomainDnsZones,SYSTEM_DOMAIN
static
DWORD
_VmDirDumpQ(
    PDEQUE  pDequeDCObjUPN
    )
{
    DWORD       dwError = 0;
    PSTR        pszFileName = VMDIR_LOG_DIR VMDIR_PATH_SEP DR_DC_LIST;
    FILE*       fDumpFile = NULL;
    DWORD       dwWritten = 0;
    PVMDIR_DR_DC_INFO pVal = NULL;
    PDEQUE_NODE pDequeTmp = NULL;
    PSTR        pszOutLine = NULL;

    fDumpFile = fopen(pszFileName, "w");
    if (fDumpFile == NULL)
    {
        BAIL_WITH_VMDIR_ERROR(dwError, VMDIR_ERROR_IO);
    }

    for (pDequeTmp = pDequeDCObjUPN->pHead; pDequeTmp != NULL; pDequeTmp = pDequeTmp->pNext)
    {
        pVal = (PVMDIR_DR_DC_INFO) pDequeTmp->pElement;
        dwError = VmDirAllocateStringPrintf(&pszOutLine, "%s,%s\n", pVal->pszUPN, pVal->pszSite);
        dwWritten = (DWORD) fwrite(
                            pszOutLine,
                            sizeof(char),
                            VmDirStringLenA(pszOutLine),
                            fDumpFile);
        if (dwWritten != VmDirStringLenA(pszOutLine))
        {
            BAIL_WITH_VMDIR_ERROR(dwError, VMDIR_ERROR_IO);
        }
    }

cleanup:
    if (fDumpFile)
    {
        fclose(fDumpFile);
    }
    VMDIR_SAFE_FREE_MEMORY(pszOutLine);

    return dwError;
error:
    goto cleanup;
}

// Delete ManagedServiceAccount entries that are associated with any of the domain controllers
// udner cn=Managed Service Accounts,SYSTEM_DOMAIN
static
DWORD
_VmDirDeleteManagedService(
    PDEQUE  pDequeDCObjUPN
    )
{
    DWORD   dwError = 0;
    int     i = 0;
    PVDIR_ATTRIBUTE pAttrUPN = NULL;
    PSTR    pszManagedServiceAccountContainerDn = NULL;
    VDIR_ENTRY_ARRAY entryArray = {0};
    PDEQUE_NODE pDequeTmp = NULL;

    dwError = VmDirAllocateStringPrintf(
        &pszManagedServiceAccountContainerDn,
        "cn=%s,%s",
        VMDIR_MSAS_RDN_VAL,
        gVmdirServerGlobals.systemDomainDN.lberbv_val);
    BAIL_ON_VMDIR_ERROR(dwError);

    dwError = VmDirSimpleEqualFilterInternalSearch(
        pszManagedServiceAccountContainerDn,
        LDAP_SCOPE_ONE,
        ATTR_OBJECT_CLASS,
        OC_MANAGED_SERVICE_ACCOUNT,
        &entryArray);
    BAIL_ON_VMDIR_ERROR(dwError);

    if (entryArray.iSize > 0)
    {
        for (i = 0; i < entryArray.iSize; i++)
        {
            pDequeTmp = NULL;

            pAttrUPN = VmDirFindAttrByName(&entryArray.pEntry[i], ATTR_KRB_UPN);

            for (pDequeTmp = pDequeDCObjUPN->pHead; pDequeTmp != NULL; pDequeTmp = pDequeTmp->pNext)
            {
                if (VmDirStringCaseStrA(pAttrUPN->vals[0].lberbv_val, pDequeTmp->pElement) != NULL)
                {
                    dwError = VmDirDeleteEntry(&entryArray.pEntry[i]);
                    BAIL_ON_VMDIR_ERROR(dwError);
                    break;
                }
            }
        }
    }

cleanup:
    VmDirFreeEntryArrayContent(&entryArray);
    VMDIR_SAFE_FREE_MEMORY(pszManagedServiceAccountContainerDn);

    return dwError;
error:
    goto cleanup;
}
