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

#include "includes.h"

#define LWCA_SECURITY_CONFIG "./test-mutentcasecurity-config/test-mutentcasecurity-config.json"
#define LWCA_SECURITY_KEY    "security"

VOID
Test_LwCASecurityInitCtx(
    VOID **state
    )
{
    DWORD dwError = 0;
    PLWCA_JSON_OBJECT pJson =  NULL;
    PLWCA_JSON_OBJECT pSecurityConfig =  NULL;

    dwError = LwCAJsonLoadObjectFromFile(LWCA_SECURITY_CONFIG, &pJson);
    assert_int_equal(dwError, 0);

    dwError = LwCAJsonGetObjectFromKey(
                  pJson,
                  FALSE,
                  LWCA_SECURITY_KEY,
                  &pSecurityConfig);
    assert_int_equal(dwError, 0);

    dwError = LwCASecurityInitCtx(pSecurityConfig);
    assert_int_equal(dwError, 0);

    LwCAJsonCleanupObject(pJson);
}
