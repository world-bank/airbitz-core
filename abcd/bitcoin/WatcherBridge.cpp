/*
 *  Copyright (c) 2014, Airbitz
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms are permitted provided that
 *  the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice, this
 *  list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  3. Redistribution or use of modified source code requires the express written
 *  permission of Airbitz Inc.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  The views and conclusions contained in the software and documentation are those
 *  of the authors and should not be interpreted as representing official policies,
 *  either expressed or implied, of the Airbitz Project.
 */

#include "WatcherBridge.hpp"
#include "Broadcast.hpp"
#include "picker.hpp"
#include "Testnet.hpp"
#include "../General.hpp"
#include "../util/Util.hpp"
#include <bitcoin/watcher.hpp> // Includes the rest of the stack
#include <algorithm>
#include <list>
#include <unordered_map>

namespace abcd {

#define FALLBACK_OBELISK "tcp://obelisk.airbitz.co:9091"
#define TESTNET_OBELISK "tcp://obelisk-testnet.airbitz.co:9091"
#define NO_AB_FEES

#define AB_MIN(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })
#define AB_MAX(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

struct PendingSweep
{
    bc::payment_address address;
    abcd::wif_key key;
    bool done;

    tABC_Sweep_Done_Callback fCallback;
    void *pData;
};

struct WatcherInfo
{
    abcd::watcher *watcher;
    std::set<std::string> addresses;
    std::list<PendingSweep> sweeping;

    // Callback:
    tABC_BitCoin_Event_Callback fAsyncCallback;
    void *pData;
    tABC_WalletID wallet;
};

typedef std::string WalletUUID;
static std::map<WalletUUID, WatcherInfo*> watchers_;

// The last obelisk server we connected to:
static unsigned gLastObelisk = 0;

static tABC_CC     ABC_BridgeDoSweep(WatcherInfo *watcherInfo, PendingSweep& sweep, tABC_Error *pError);
static void        ABC_BridgeQuietCallback(WatcherInfo *watcherInfo);
static void        ABC_BridgeTxCallback(WatcherInfo *watcherInfo, const libbitcoin::transaction_type& tx, tABC_BitCoin_Event_Callback fAsyncBitCoinEventCallback, void *pData);
static tABC_CC     ABC_BridgeExtractOutputs(abcd::watcher *watcher, abcd::unsigned_transaction_type *utx, std::string malleableId, tABC_UnsignedTx *pUtx, tABC_Error *pError);
static tABC_CC     ABC_BridgeTxErrorHandler(abcd::unsigned_transaction_type *utx, tABC_Error *pError);
static void        ABC_BridgeAppendOutput(bc::transaction_output_list& outputs, uint64_t amount, const bc::payment_address &addr);
static bc::script_type ABC_BridgeCreateScriptHash(const bc::short_hash &script_hash);
static bc::script_type ABC_BridgeCreatePubKeyHash(const bc::short_hash &pubkey_hash);
static uint64_t    ABC_BridgeCalcAbFees(uint64_t amount, tABC_GeneralInfo *pInfo);
static uint64_t    ABC_BridgeCalcMinerFees(size_t tx_size, tABC_GeneralInfo *pInfo, uint64_t amountSatoshi);
static std::string ABC_BridgeWatcherFile(const char *szWalletUUID);
static tABC_CC     ABC_BridgeWatcherLoad(WatcherInfo *watcherInfo, tABC_Error *pError);
static void        ABC_BridgeWatcherSerializeAsync(WatcherInfo *watcherInfo);
static void        *ABC_BridgeWatcherSerialize(void *pData);
static std::string ABC_BridgeNonMalleableTxId(bc::transaction_type tx);

tABC_CC ABC_BridgeSweepKey(tABC_WalletID self,
                           tABC_U08Buf key,
                           bool compressed,
                           tABC_Sweep_Done_Callback fCallback,
                           void *pData,
                           tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    bc::ec_secret ec_key;
    bc::ec_point ec_addr;
    bc::payment_address address;
    WatcherInfo *watcherInfo = NULL;
    PendingSweep sweep;

    auto row = watchers_.find(self.szUUID);
    ABC_CHECK_ASSERT(row != watchers_.end(), ABC_CC_Error, "Unable find watcher");
    watcherInfo = row->second;

    // Decode key and address:
    ABC_CHECK_ASSERT(ABC_BUF_SIZE(key) == ec_key.size(),
        ABC_CC_Error, "Bad key size");
    std::copy(key.p, key.end, ec_key.data());
    ec_addr = bc::secret_to_public_key(ec_key, compressed);
    address.set(pubkeyVersion(), bc::bitcoin_short_hash(ec_addr));

    // Start the sweep:
    sweep.address = address;
    sweep.key = abcd::wif_key{ec_key, compressed};
    sweep.done = false;
    sweep.fCallback = fCallback;
    sweep.pData = pData;
    watcherInfo->sweeping.push_back(sweep);
    watcherInfo->watcher->watch_address(address);

exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherStart(tABC_WalletID self,
                               tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    WatcherInfo *watcherInfo = NULL;

    auto row = watchers_.find(self.szUUID);
    if (row != watchers_.end()) {
        ABC_DebugLog("Watcher %s already initialized\n", self.szUUID);
        goto exit;
    }

    watcherInfo = new WatcherInfo();
    watcherInfo->watcher = new abcd::watcher();

    ABC_CHECK_RET(ABC_WalletIDCopy(&watcherInfo->wallet, self, pError));

    ABC_BridgeWatcherLoad(watcherInfo, pError);
    watchers_[self.szUUID] = watcherInfo;
exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherLoop(const char *szWalletUUID,
                              tABC_BitCoin_Event_Callback fAsyncCallback,
                              void *pData,
                              tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    WatcherInfo *watcherInfo = NULL;
    abcd::watcher::block_height_callback heightCallback;
    abcd::watcher::tx_callback txCallback;
    abcd::watcher::tx_sent_callback sendCallback;
    abcd::watcher::quiet_callback on_quiet;
    abcd::watcher::fail_callback failCallback;

    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        ABC_DebugLog("Watcher %s does not exist\n", szWalletUUID);
        goto exit;
    }

    watcherInfo = row->second;
    watcherInfo->fAsyncCallback = fAsyncCallback;
    watcherInfo->pData = pData;

    txCallback = [watcherInfo, fAsyncCallback, pData] (const libbitcoin::transaction_type& tx)
    {
        ABC_BridgeTxCallback(watcherInfo, tx, fAsyncCallback, pData);
    };
    watcherInfo->watcher->set_tx_callback(txCallback);

    heightCallback = [watcherInfo, fAsyncCallback, pData](const size_t height)
    {
        tABC_Error error;
        ABC_TxBlockHeightUpdate(height, fAsyncCallback, pData, &error);
        ABC_BridgeWatcherSerializeAsync(watcherInfo);
    };
    watcherInfo->watcher->set_height_callback(heightCallback);

    on_quiet = [watcherInfo]()
    {
        ABC_BridgeQuietCallback(watcherInfo);
    };
    watcherInfo->watcher->set_quiet_callback(on_quiet);

    failCallback = [watcherInfo]()
    {
        tABC_Error error;
        ABC_BridgeWatcherConnect(watcherInfo->wallet.szUUID, &error);
    };
    watcherInfo->watcher->set_fail_callback(failCallback);

    row->second->watcher->loop();
exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherConnect(const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_GeneralInfo *ppInfo = NULL;
    WatcherInfo *watcherInfo = NULL;
    const char *szServer = FALLBACK_OBELISK;

    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        ABC_DebugLog("Watcher %s does not exist\n", szWalletUUID);
        goto exit;
    }
    watcherInfo = row->second;

    // Pick a server:
    if (isTestnet())
    {
        szServer = TESTNET_OBELISK;
    }
    else if (ABC_CC_Ok == ABC_GeneralGetInfo(&ppInfo, pError) &&
        0 < ppInfo->countObeliskServers)
    {
        ++gLastObelisk;
        if (ppInfo->countObeliskServers <= gLastObelisk)
            gLastObelisk = 0;
        szServer = ppInfo->aszObeliskServers[gLastObelisk];
    }

    // Connect:
    ABC_DebugLog("Connecting to %s\n", szServer);
    watcherInfo->watcher->connect(szServer);

exit:
    ABC_GeneralFreeInfo(ppInfo);
    return cc;
}

tABC_CC ABC_BridgeWatchAddr(const char *szWalletUUID,
                            const char *pubAddress,
                            tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    auto row = watchers_.find(szWalletUUID);

    ABC_DebugLog("Watching %s for %s\n", pubAddress, szWalletUUID);
    bc::payment_address addr;

    if (row == watchers_.end())
    {
        goto exit;
    }

    if (!addr.set_encoded(pubAddress))
    {
        cc = ABC_CC_Error;
        ABC_DebugLog("Invalid pubAddress %s\n", pubAddress);
        goto exit;
    }
    row->second->addresses.insert(pubAddress);
    row->second->watcher->watch_address(addr);
exit:
    return cc;
}

tABC_CC ABC_BridgeWatchPath(const char *szWalletUUID, char **szPath,
                            tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    std::string filepath(
            ABC_BridgeWatcherFile(szWalletUUID));
    ABC_STRDUP(*szPath, filepath.c_str());
exit:
    return cc;
}

tABC_CC ABC_BridgePrioritizeAddress(const char *szWalletUUID,
                                    const char *szAddress,
                                    tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    auto row = watchers_.find(szWalletUUID);
    bc::payment_address addr;

    if (row == watchers_.end())
    {
        goto exit;
    }

    if (szAddress)
    {
        if (!addr.set_encoded(szAddress))
        {
            cc = ABC_CC_Error;
            ABC_DebugLog("Invalid szAddress %s\n", szAddress);
            goto exit;
        }
        row->second->watcher->prioritize_address(addr);
    }
    else
    {
        row->second->watcher->prioritize_address(addr);
    }
exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherDisconnect(const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        ABC_DebugLog("Watcher %s does not exist\n", szWalletUUID);
        goto exit;
    }
    row->second->watcher->disconnect();
exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherStop(const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        ABC_DebugLog("Watcher %s does not exist\n", szWalletUUID);
        goto exit;
    }
    row->second->watcher->disconnect();
    row->second->watcher->stop();
exit:
    return cc;
}

tABC_CC ABC_BridgeWatcherDelete(const char *szWalletUUID, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    WatcherInfo *watcherInfo = NULL;

    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        ABC_DebugLog("Watcher %s does not exist\n", szWalletUUID);
        goto exit;
    }
    watcherInfo = row->second;

    // Remove info from map:
    watchers_.erase(szWalletUUID);

    // Delete watcher:
    ABC_BridgeWatcherSerialize(watcherInfo);
    if (watcherInfo->watcher != NULL) {
        delete watcherInfo->watcher;
    }
    watcherInfo->watcher = NULL;

    // Delete info:
    ABC_WalletIDFree(watcherInfo->wallet);
    if (watcherInfo != NULL) {
        delete watcherInfo;
    }

exit:
    return cc;
}

tABC_CC ABC_BridgeTxMake(tABC_TxSendInfo *pSendInfo,
                         char **addresses, int addressCount,
                         char *changeAddress,
                         tABC_UnsignedTx *pUtx,
                         tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_GeneralInfo *ppInfo = NULL;
    bc::payment_address change, ab, dest;
    abcd::fee_schedule schedule;
    abcd::unsigned_transaction_type *utx;
    bc::transaction_output_list outputs;
    uint64_t totalAmountSatoshi = 0, abFees = 0, minerFees = 0;
    std::vector<bc::payment_address> addresses_;

    // Find a watcher to use
    auto row = watchers_.find(pSendInfo->wallet.szUUID);
    ABC_CHECK_ASSERT(row != watchers_.end(),
        ABC_CC_Error, "Unable find watcher");

    // Alloc a new utx
    utx = new abcd::unsigned_transaction_type();
    ABC_CHECK_ASSERT(utx != NULL,
        ABC_CC_NULLPtr, "Unable alloc unsigned_transaction_type");

    // Update general info before send
    ABC_CHECK_RET(ABC_GeneralUpdateInfo(pError));
    // Fetch Info to calculate fees
    ABC_CHECK_RET(ABC_GeneralGetInfo(&ppInfo, pError));
    // Create payment_addresses
    ABC_CHECK_ASSERT(addressCount > 0,
        ABC_CC_Error, "No addresses supplied");
    for (int i = 0; i < addressCount; ++i)
    {
        bc::payment_address pa;
        ABC_CHECK_ASSERT(true == pa.set_encoded(addresses[i]),
            ABC_CC_Error, "Bad source address");
        addresses_.push_back(pa);
    }
    ABC_CHECK_ASSERT(true == change.set_encoded(changeAddress),
        ABC_CC_Error, "Bad change address");
    ABC_CHECK_ASSERT(true == dest.set_encoded(pSendInfo->szDestAddress),
        ABC_CC_Error, "Bad destination address");
    ABC_CHECK_ASSERT(true == ab.set_encoded(ppInfo->pAirBitzFee->szAddresss),
        ABC_CC_Error, "Bad ABV address");

    schedule.satoshi_per_kb = ppInfo->countMinersFees;
    totalAmountSatoshi = pSendInfo->pDetails->amountSatoshi;

    if (!pSendInfo->bTransfer)
    {
        // Calculate AB Fees
        abFees = ABC_BridgeCalcAbFees(pSendInfo->pDetails->amountSatoshi, ppInfo);

        // Add in miners fees
        if (abFees > 0)
        {
            pSendInfo->pDetails->amountFeesAirbitzSatoshi = abFees;
            // Output to Airbitz
            ABC_BridgeAppendOutput(outputs, abFees, ab);
            // Increment total tx amount to account for AB fees
            totalAmountSatoshi += abFees;
        }
    }
    // Output to  Destination Address
    ABC_BridgeAppendOutput(outputs, pSendInfo->pDetails->amountSatoshi, dest);

    minerFees = ABC_BridgeCalcMinerFees(bc::satoshi_raw_size(utx->tx), ppInfo, pSendInfo->pDetails->amountSatoshi);
    if (minerFees > 0)
    {
        // If there are miner fees, increase totalSatoshi
        pSendInfo->pDetails->amountFeesMinersSatoshi = minerFees;
        totalAmountSatoshi += minerFees;
    }
    // Set the fees in the send details
    pSendInfo->pDetails->amountFeesAirbitzSatoshi = abFees;
    pSendInfo->pDetails->amountFeesMinersSatoshi = minerFees;
    ABC_DebugLog("Change: %s, Amount: %ld, Amount w/Fees %d\n",
                    change.encoded().c_str(),
                    pSendInfo->pDetails->amountSatoshi,
                    totalAmountSatoshi);
    if (!abcd::make_tx(*(row->second->watcher), addresses_, change,
                            totalAmountSatoshi, schedule, outputs, *utx))
    {
        ABC_CHECK_RET(ABC_BridgeTxErrorHandler(utx, pError));
    }

    pUtx->data = (void *) utx;
exit:
    ABC_GeneralFreeInfo(ppInfo);
    return cc;
}

tABC_CC ABC_BridgeTxSignSend(tABC_TxSendInfo *pSendInfo,
                             char **paPrivKey,
                             unsigned int keyCount,
                             tABC_UnsignedTx *pUtx,
                             tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;

    WatcherInfo *watcherInfo = NULL;
    abcd::unsigned_transaction_type *utx;
    std::vector<std::string> keys;
    std::string txid, malleableId;

    utx = (abcd::unsigned_transaction_type *) pUtx->data;
    auto row = watchers_.find(pSendInfo->wallet.szUUID);
    ABC_CHECK_ASSERT(row != watchers_.end(), ABC_CC_Error, "Unable find watcher");

    watcherInfo = row->second;

    for (unsigned i = 0; i < keyCount; ++i)
    {
        keys.push_back(std::string(paPrivKey[i]));
    }

    // Sign the transaction
    if (!abcd::sign_tx(*utx, keys, *watcherInfo->watcher))
    {
        ABC_CHECK_RET(ABC_BridgeTxErrorHandler(utx, pError));
    }

    // Send to the network:
    {
        bc::data_chunk raw_tx(satoshi_raw_size(utx->tx));
        bc::satoshi_save(utx->tx, raw_tx.begin());
        ABC_CHECK_NEW(broadcastTx(raw_tx), pError);
    }

    // This will mark the outputs as spent
    watcherInfo->watcher->send_tx(utx->tx);

    txid = ABC_BridgeNonMalleableTxId(utx->tx);
    ABC_STRDUP(pUtx->szTxId, txid.c_str());

    malleableId = bc::encode_hex(bc::hash_transaction(utx->tx));
    ABC_STRDUP(pUtx->szTxMalleableId, malleableId.c_str());

    ABC_BridgeWatcherSerializeAsync(watcherInfo);
    ABC_BridgeExtractOutputs(watcherInfo->watcher, utx, malleableId, pUtx,pError);

exit:
    return cc;
}

tABC_CC ABC_BridgeMaxSpendable(tABC_WalletID self,
                               const char *szDestAddress,
                               bool bTransfer,
                               uint64_t *pMaxSatoshi,
                               tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_TxSendInfo SendInfo = {{0}};
    tABC_TxDetails Details;
    tABC_GeneralInfo *ppInfo = NULL;
    tABC_UnsignedTx utx;
    tABC_CC txResp;

    char *changeAddr = NULL;
    AutoStringArray addresses;

    auto row = watchers_.find(self.szUUID);
    uint64_t total = 0;

    ABC_CHECK_ASSERT(row != watchers_.end(),
        ABC_CC_Error, "Unable find watcher");

    SendInfo.wallet = self;
    ABC_STRDUP(SendInfo.szDestAddress, szDestAddress);

    // Snag the latest general info
    ABC_CHECK_RET(ABC_GeneralGetInfo(&ppInfo, pError));
    // Fetch all the payment addresses for this wallet
    ABC_CHECK_RET(
        ABC_TxGetPubAddresses(self, &addresses.data, &addresses.size, pError));
    if (addresses.size > 0)
    {
        // This is needed to pass to the ABC_BridgeTxMake
        // It should never be used
        changeAddr = addresses.data[0];

        // Calculate total of utxos for these addresses
        ABC_DebugLog("Get UTOXs for %d\n", addresses.size);
        auto utxos = row->second->watcher->get_utxos(true);
        for (const auto& utxo: utxos)
        {
            total += utxo.value;
        }
        if (!bTransfer)
        {
            // Subtract ab tx fee
            total -= ABC_BridgeCalcAbFees(total, ppInfo);
        }
        // Subtract minimum tx fee
        total -= ABC_BridgeCalcMinerFees(0, ppInfo, total);

        SendInfo.pDetails = &Details;
        SendInfo.bTransfer = bTransfer;
        Details.amountSatoshi = total;

        // Ewwwwww, fix this to have minimal iterations
        txResp = ABC_BridgeTxMake(&SendInfo,
                                  addresses.data, addresses.size,
                                  changeAddr, &utx, pError);
        while (txResp == ABC_CC_InsufficientFunds && Details.amountSatoshi > 0)
        {
            Details.amountSatoshi -= 1;
            txResp = ABC_BridgeTxMake(&SendInfo,
                                      addresses.data, addresses.size,
                                      changeAddr, &utx, pError);
        }
        *pMaxSatoshi = AB_MAX(Details.amountSatoshi, 0);
    }
    else
    {
        *pMaxSatoshi = 0;
    }
exit:
    ABC_FREE_STR(SendInfo.szDestAddress);
    ABC_GeneralFreeInfo(ppInfo);
    return cc;
}

tABC_CC
ABC_BridgeTxHeight(const char *szWalletUUID, const char *szTxId, unsigned int *height, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    int height_;
    bc::hash_digest txId;
    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        cc = ABC_CC_Synchronizing;
        goto exit;
    }
    txId = bc::decode_hash(szTxId);
    if (!row->second->watcher->get_tx_height(txId, height_))
    {
        cc = ABC_CC_Synchronizing;
    }
    *height = height_;
exit:
    return cc;
}

tABC_CC
ABC_BridgeTxBlockHeight(const char *szWalletUUID, unsigned int *height, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        cc = ABC_CC_Synchronizing;
        goto exit;
    }
    *height = row->second->watcher->get_last_block_height();
    if (*height == 0)
    {
        cc = ABC_CC_Synchronizing;
    }
exit:
    return cc;
}

tABC_CC ABC_BridgeTxDetails(const char *szWalletUUID, const char *szTxID,
                            tABC_TxOutput ***paOutputs, unsigned int *pCount,
                            int64_t *pAmount, int64_t *pFees, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_TxOutput **paInArr = NULL;
    tABC_TxOutput **paOutArr = NULL;
    tABC_TxOutput **farr = NULL;
    unsigned int outCount = 0;
    unsigned int inCount = 0;
    unsigned int totalCount = 0;

    ABC_CHECK_RET(ABC_BridgeTxDetailsSplit(szWalletUUID, szTxID,
                                           &paInArr, &inCount,
                                           &paOutArr, &outCount,
                                           pAmount, pFees, pError));
    farr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * (inCount + outCount));
    totalCount = outCount + inCount;
    for (unsigned i = 0; i < totalCount; ++i) {
        if (i < inCount) {
            farr[i] = paInArr[i];
            paInArr[i] = NULL;
        } else {
            farr[i] = paOutArr[i - inCount];
            paOutArr[i - inCount] = NULL;
        }
    }
    *paOutputs = farr;
    *pCount = totalCount;
    farr = NULL;
exit:
    ABC_TxFreeOutputs(farr, inCount + outCount);
    ABC_TxFreeOutputs(paInArr, inCount);
    ABC_TxFreeOutputs(paOutArr, outCount);
    return cc;
}

tABC_CC ABC_BridgeTxDetailsSplit(const char *szWalletUUID, const char *szTxID,
                                 tABC_TxOutput ***paInputs, unsigned int *pInCount,
                                 tABC_TxOutput ***paOutputs, unsigned int *pOutCount,
                                 int64_t *pAmount, int64_t *pFees,
                                 tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    WatcherInfo *watcherInfo = NULL;
    tABC_TxOutput **paInArr = NULL;
    tABC_TxOutput **paOutArr = NULL;
    bc::transaction_type tx;
    unsigned int idx = 0, iCount = 0, oCount = 0;
    int64_t fees = 0;
    int64_t totalInSatoshi = 0, totalOutSatoshi = 0, totalMeSatoshi = 0, totalMeInSatoshi = 0;

    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
    {
        cc = ABC_CC_Synchronizing;
        goto exit;
    }
    bc::hash_digest txid;
    txid = bc::decode_hash(szTxID);

    watcherInfo = row->second;
    tx = watcherInfo->watcher->find_tx(txid);

    idx = 0;
    iCount = tx.inputs.size();
    paInArr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * iCount);
    for (auto i : tx.inputs)
    {
        bc::payment_address addr;
        bc::extract(addr, i.script);
        auto prev = i.previous_output;

        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = true;
        ABC_STRDUP(out->szTxId, bc::encode_hex(prev.hash).c_str());
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());

        auto tx = watcherInfo->watcher->find_tx(prev.hash);
        if (prev.index < tx.outputs.size())
        {
            out->value = tx.outputs[prev.index].value;
            totalInSatoshi += tx.outputs[prev.index].value;
            auto row = watcherInfo->addresses.find(addr.encoded());
            if  (row != watcherInfo->addresses.end())
                totalMeInSatoshi += tx.outputs[prev.index].value;
        } else {
            out->value = 0;
        }
        paInArr[idx] = out;
        idx++;
    }

    idx = 0;
    oCount = tx.outputs.size();
    paOutArr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * oCount);
    for (auto o : tx.outputs)
    {
        bc::payment_address addr;
        bc::extract(addr, o.script);
        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = false;
        out->value = o.value;
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());
        ABC_STRDUP(out->szTxId, szTxID);

        // Do we own this address?
        auto row = watcherInfo->addresses.find(addr.encoded());
        if  (row != watcherInfo->addresses.end())
        {
            totalMeSatoshi += o.value;
        }
        totalOutSatoshi += o.value;
        paOutArr[idx] = out;
        idx++;
    }
    fees = totalInSatoshi - totalOutSatoshi;
    totalMeSatoshi -= totalMeInSatoshi;

    *paInputs = paInArr;
    *pInCount = iCount;
    *paOutputs = paOutArr;
    *pOutCount = oCount;
    *pAmount = totalMeSatoshi;
    *pFees = fees;
    paInArr = NULL;
    paOutArr = NULL;
exit:
    ABC_TxFreeOutputs(paInArr, iCount);
    ABC_TxFreeOutputs(paOutArr, oCount);
    return cc;
}

/**
 * Filters a transaction list, removing any that aren't found in the
 * watcher database.
 * @param aTransactions The array to filter. This will be modified in-place.
 * @param pCount        The array length. This will be updated upon return.
 */
tABC_CC ABC_BridgeFilterTransactions(const char *szWalletUUID,
                                     tABC_TxInfo **aTransactions,
                                     unsigned int *pCount,
                                     tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    WatcherInfo *watcherInfo = NULL;
    tABC_TxInfo *const *end = aTransactions + *pCount;
    tABC_TxInfo *const *si = aTransactions;
    tABC_TxInfo **di = aTransactions;

    auto row = watchers_.find(szWalletUUID);
    ABC_CHECK_ASSERT(row != watchers_.end(),
        ABC_CC_Synchronizing, "Unable to find watcher");
    watcherInfo = row->second;

    while (si < end)
    {
        tABC_TxInfo *pTx = *si++;

        int height;
        bc::hash_digest txid;
        txid = bc::decode_hash(pTx->szMalleableTxId);
        if (watcherInfo->watcher->get_tx_height(txid, height))
        {
            *di++ = pTx;
        }
        else
        {
            ABC_TxFreeTransaction(pTx);
        }
    }
    *pCount = di - aTransactions;

exit:
    return cc;
}

static
tABC_CC ABC_BridgeDoSweep(WatcherInfo *watcherInfo,
                          PendingSweep& sweep,
                          tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    char *szID = NULL;
    char *szAddress = NULL;
    bc::payment_address to_address;
    uint64_t funds = 0;
    abcd::unsigned_transaction utx;
    bc::transaction_output_type output;
    abcd::key_table keys;
    std::string malTxId, txId;

    // Find utxos for this address:
    auto utxos = watcherInfo->watcher->get_utxos(sweep.address);

    // Bail out if there are no funds to sweep:
    if (!utxos.size())
    {
        // Tell the GUI if there were funds in the past:
        if (watcherInfo->watcher->db().has_history(sweep.address))
        {
            if (sweep.fCallback)
            {
                sweep.fCallback(ABC_CC_Ok, NULL, 0);
            }
            else
            {
                if (watcherInfo->fAsyncCallback)
                {
                    tABC_AsyncBitCoinInfo info;
                    info.eventType = ABC_AsyncEventType_IncomingSweep;
                    info.sweepSatoshi = 0;
                    info.szTxID = NULL;
                    watcherInfo->fAsyncCallback(&info);
                }
            }
            sweep.done = true;
        }
        return ABC_CC_Ok;
    }

    // There are some utxos, so send them to ourselves:
    tABC_TxDetails details;
    memset(&details, 0, sizeof(tABC_TxDetails));
    details.amountSatoshi = 0;
    details.amountCurrency = 0;
    details.amountFeesAirbitzSatoshi = 0;
    details.amountFeesMinersSatoshi = 0;
    details.szName = const_cast<char*>("");
    details.szCategory = const_cast<char*>("");
    details.szNotes = const_cast<char*>("");
    details.attributes = 0x2;

    // Create a new receive request:
    ABC_CHECK_RET(ABC_TxCreateReceiveRequest(watcherInfo->wallet,
        &details, &szID, false, pError));
    ABC_CHECK_RET(ABC_TxGetRequestAddress(watcherInfo->wallet, szID,
        &szAddress, pError));
    to_address.set_encoded(szAddress);

    // Build a transaction:
    utx.tx.version = 1;
    utx.tx.locktime = 0;
    for (auto &utxo : utxos)
    {
        bc::transaction_input_type input;
        input.sequence = 0xffffffff;
        input.previous_output = utxo.point;
        funds += utxo.value;
        utx.tx.inputs.push_back(input);
    }
    ABC_CHECK_ASSERT(10500 <= funds, ABC_CC_InsufficientFunds, "Not enough funds");
    funds -= 10000;
    output.value = funds;
    output.script = abcd::build_pubkey_hash_script(to_address.hash());
    utx.tx.outputs.push_back(output);

    // Now sign that:
    keys[sweep.address] = sweep.key;
    ABC_CHECK_SYS(abcd::gather_challenges(utx, *watcherInfo->watcher), "gather_challenges");
    ABC_CHECK_SYS(abcd::sign_tx(utx, keys), "sign_tx");

    // Send:
    {
        bc::data_chunk raw_tx(satoshi_raw_size(utx.tx));
        bc::satoshi_save(utx.tx, raw_tx.begin());
        ABC_CHECK_NEW(broadcastTx(raw_tx), pError);
    }

    // Save the transaction in the database:
    malTxId = bc::encode_hex(bc::hash_transaction(utx.tx));
    txId = ABC_BridgeNonMalleableTxId(utx.tx);
    ABC_CHECK_RET(ABC_TxSweepSaveTransaction(watcherInfo->wallet,
        txId.c_str(), malTxId.c_str(), funds, &details, pError));

    // Done:
    if (sweep.fCallback)
    {
        sweep.fCallback(ABC_CC_Ok, txId.c_str(), output.value);
    }
    else
    {
        if (watcherInfo->fAsyncCallback)
        {
            tABC_AsyncBitCoinInfo info;
            info.eventType = ABC_AsyncEventType_IncomingSweep;
            info.sweepSatoshi = output.value;
            ABC_STRDUP(info.szTxID, txId.c_str());
            watcherInfo->fAsyncCallback(&info);
            ABC_FREE_STR(info.szTxID);
        }
    }
    sweep.done = true;
    watcherInfo->watcher->send_tx(utx.tx);

exit:
    ABC_FREE_STR(szID);
    ABC_FREE_STR(szAddress);

    return cc;
}

static
void ABC_BridgeQuietCallback(WatcherInfo *watcherInfo)
{
    // If we are sweeping any keys, do that now:
    for (auto& sweep: watcherInfo->sweeping)
    {
        tABC_CC cc;
        tABC_Error error;

        cc = ABC_BridgeDoSweep(watcherInfo, sweep, &error);
        if (cc != ABC_CC_Ok)
        {
            if (sweep.fCallback)
                sweep.fCallback(cc, NULL, 0);
            sweep.done = true;
        }
    }

    // Remove completed ones:
    watcherInfo->sweeping.remove_if([](const PendingSweep& sweep) {
        return sweep.done; });
}

static
void ABC_BridgeTxCallback(WatcherInfo *watcherInfo, const libbitcoin::transaction_type& tx,
                          tABC_BitCoin_Event_Callback fAsyncBitCoinEventCallback,
                          void *pData)
{
    tABC_CC cc = ABC_CC_Ok;
    tABC_Error error;
    tABC_Error *pError = &error;
    int64_t fees = 0;
    int64_t totalInSatoshi = 0, totalOutSatoshi = 0, totalMeSatoshi = 0, totalMeInSatoshi = 0;
    tABC_TxOutput **iarr = NULL, **oarr = NULL;
    unsigned int idx = 0, iCount = 0, oCount = 0;
    std::string txId, malTxId;

    if (watcherInfo == NULL)
    {
        cc = ABC_CC_Error;
        goto exit;
    }

    txId = ABC_BridgeNonMalleableTxId(tx);
    malTxId = bc::encode_hex(bc::hash_transaction(tx));

    idx = 0;
    iCount = tx.inputs.size();
    iarr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * iCount);
    for (auto i : tx.inputs)
    {
        bc::payment_address addr;
        bc::extract(addr, i.script);
        auto prev = i.previous_output;

        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = true;
        ABC_STRDUP(out->szTxId, bc::encode_hex(prev.hash).c_str());
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());

        // Check prevouts for values
        auto tx = watcherInfo->watcher->find_tx(prev.hash);
        if (prev.index < tx.outputs.size())
        {
            out->value = tx.outputs[prev.index].value;
            totalInSatoshi += tx.outputs[prev.index].value;
            auto row = watcherInfo->addresses.find(addr.encoded());
            if  (row != watcherInfo->addresses.end())
                totalMeInSatoshi += tx.outputs[prev.index].value;
        }
        iarr[idx] = out;
        idx++;
    }

    idx = 0;
    oCount = tx.outputs.size();
    oarr = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput *) * oCount);
    for (auto o : tx.outputs)
    {
        bc::payment_address addr;
        bc::extract(addr, o.script);
        // Create output
        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = false;
        out->value = o.value;
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());
        ABC_STRDUP(out->szTxId, malTxId.c_str());

        // Do we own this address?
        auto row = watcherInfo->addresses.find(addr.encoded());
        if  (row != watcherInfo->addresses.end())
        {
            totalMeSatoshi += o.value;
        }
        totalOutSatoshi += o.value;

        oarr[idx] = out;
        idx++;
    }
    if (totalMeSatoshi == 0 && totalMeInSatoshi == 0)
    {
        ABC_DebugLog("values == 0, this tx does not concern me.\n");
        goto exit;
    }
    fees = totalInSatoshi - totalOutSatoshi;
    totalMeSatoshi -= totalMeInSatoshi;

    ABC_DebugLog("calling ABC_TxReceiveTransaction\n");
    ABC_DebugLog("Total Me: %d, Total In: %d, Total Out: %d, Fees: %d\n",
                    totalMeSatoshi, totalInSatoshi, totalOutSatoshi, fees);
    ABC_CHECK_RET(
        ABC_TxReceiveTransaction(
            watcherInfo->wallet,
            totalMeSatoshi, fees,
            iarr, iCount,
            oarr, oCount,
            txId.c_str(), malTxId.c_str(),
            fAsyncBitCoinEventCallback,
            pData,
            &error));
    ABC_BridgeWatcherSerializeAsync(watcherInfo);
exit:
    ABC_FREE(oarr);
    ABC_FREE(iarr);
}

static tABC_CC
ABC_BridgeExtractOutputs(abcd::watcher *watcher, abcd::unsigned_transaction_type *utx,
                         std::string malleableId, tABC_UnsignedTx *pUtx, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    pUtx->countOutputs = utx->tx.inputs.size() + utx->tx.outputs.size();
    pUtx->aOutputs = (tABC_TxOutput **) malloc(sizeof(tABC_TxOutput) * pUtx->countOutputs);
    int i = 0;
    for (auto& input : utx->tx.inputs)
    {
        auto prev = input.previous_output;
        bc::payment_address addr;
        bc::extract(addr, input.script);

        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = true;
        ABC_STRDUP(out->szTxId, bc::encode_hex(prev.hash).c_str());
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());

        auto tx = watcher->find_tx(prev.hash);
        if (prev.index < tx.outputs.size())
        {
            out->value = tx.outputs[prev.index].value;
        }
        pUtx->aOutputs[i] = out;
        i++;
    }
    for (auto& output : utx->tx.outputs)
    {
        bc::payment_address addr;
        bc::extract(addr, output.script);

        tABC_TxOutput *out = (tABC_TxOutput *) malloc(sizeof(tABC_TxOutput));
        out->input = false;
        out->value = output.value;
        ABC_STRDUP(out->szTxId, malleableId.c_str());
        ABC_STRDUP(out->szAddress, addr.encoded().c_str());

        pUtx->aOutputs[i] = out;
        i++;
    }
exit:
    return cc;
}

static
tABC_CC ABC_BridgeTxErrorHandler(abcd::unsigned_transaction_type *utx, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    switch (utx->code)
    {
        case abcd::insufficent_funds:
            ABC_RET_ERROR(ABC_CC_InsufficientFunds, "Insufficent funds.");
        case abcd::invalid_key:
            ABC_RET_ERROR(ABC_CC_Error, "Invalid address.");
        case abcd::invalid_sig:
            ABC_RET_ERROR(ABC_CC_Error, "Unable to sign.");
        default:
            break;
    }
exit:
    return cc;
}

static
void ABC_BridgeAppendOutput(bc::transaction_output_list& outputs, uint64_t amount, const bc::payment_address &addr)
{
    bc::transaction_output_type output;
    output.value = amount;
    if (addr.version() == pubkeyVersion())
    {
        output.script = ABC_BridgeCreatePubKeyHash(addr.hash());
    }
    else if (addr.version() == scriptVersion())
    {
        output.script = ABC_BridgeCreateScriptHash(addr.hash());
    }
    outputs.push_back(output);
}

static
bc::script_type ABC_BridgeCreateScriptHash(const bc::short_hash &script_hash)
{
    bc::script_type result;
    result.push_operation({bc::opcode::hash160, bc::data_chunk()});
    result.push_operation({bc::opcode::special, bc::data_chunk(script_hash.begin(), script_hash.end())});
    result.push_operation({bc::opcode::equal, bc::data_chunk()});
    return result;
}

static
bc::script_type ABC_BridgeCreatePubKeyHash(const bc::short_hash &pubkey_hash)
{
    bc::script_type result;
    result.push_operation({bc::opcode::dup, bc::data_chunk()});
    result.push_operation({bc::opcode::hash160, bc::data_chunk()});
    result.push_operation({bc::opcode::special,
        bc::data_chunk(pubkey_hash.begin(), pubkey_hash.end())});
    result.push_operation({bc::opcode::equalverify, bc::data_chunk()});
    result.push_operation({bc::opcode::checksig, bc::data_chunk()});
    return result;
}

static
uint64_t ABC_BridgeCalcAbFees(uint64_t amount, tABC_GeneralInfo *pInfo)
{

#ifdef NO_AB_FEES
    return 0;
#else
    uint64_t abFees =
        (uint64_t) ((double) amount *
                    (pInfo->pAirBitzFee->percentage * 0.01));
    abFees = AB_MAX(pInfo->pAirBitzFee->minSatoshi, abFees);
    abFees = AB_MIN(pInfo->pAirBitzFee->maxSatoshi, abFees);

    return abFees;
#endif
}

static
uint64_t ABC_BridgeCalcMinerFees(size_t tx_size, tABC_GeneralInfo *pInfo, uint64_t amountSatoshi)
{
    // Look up the size-based fees from the table:
    uint64_t sizeFee = 0;
    if (pInfo->countMinersFees > 0)
    {
        for (unsigned i = 0; i < pInfo->countMinersFees; ++i)
        {
            if (tx_size <= pInfo->aMinersFees[i]->sizeTransaction)
            {
                sizeFee = pInfo->aMinersFees[i]->amountSatoshi;
                break;
            }
        }
    }
    if (!sizeFee)
        return 0;

    // The amount-based fee is 0.1% of total funds sent:
    uint64_t amountFee = amountSatoshi / 1000;

    // Clamp the amount fee between 10% and 100% of the size-based fee:
    uint64_t minFee = sizeFee / 10;
    amountFee = std::max(amountFee, minFee);
    amountFee = std::min(amountFee, sizeFee);

    // Make the result an integer multiple of the minimum fee:
    return amountFee - amountFee % minFee;
}

static
std::string ABC_BridgeWatcherFile(const char *szWalletUUID)
{
    char *szDirName = NULL;
    tABC_Error error;
    ABC_WalletGetDirName(&szDirName, szWalletUUID, &error);

    std::string filepath;
    filepath.append(std::string(szDirName));
    filepath.append("/watcher.ser");
    return filepath;
}

static
tABC_CC ABC_BridgeWatcherLoad(WatcherInfo *watcherInfo, tABC_Error *pError)
{
    tABC_CC cc = ABC_CC_Ok;
    uint8_t *pData = NULL;
    std::streampos size;

    std::string filepath(
            ABC_BridgeWatcherFile(watcherInfo->wallet.szUUID));

    struct stat buffer;
    if (stat(filepath.c_str(), &buffer) == 0)
    {
        std::ifstream file(filepath, std::ios::in | std::ios::binary | std::ios::ate);
        ABC_CHECK_ASSERT(file.is_open() == true, ABC_CC_Error, "Unable to open file for loading");

        size = file.tellg();
        pData = new uint8_t[size];
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char *>(pData), size);
        file.close();

        ABC_CHECK_ASSERT(watcherInfo->watcher->load(bc::data_chunk(pData, pData + size)) == true,
            ABC_CC_Error, "Unable to load serialized state\n");
    }
exit:
    ABC_FREE(pData);
    return cc;
}

static
void ABC_BridgeWatcherSerializeAsync(WatcherInfo *watcherInfo)
{
    pthread_t handle;
    if (!pthread_create(&handle, NULL, ABC_BridgeWatcherSerialize, watcherInfo))
    {
        pthread_detach(handle);
    }
}

static
void *ABC_BridgeWatcherSerialize(void *pData)
{
    bc::data_chunk db;
    WatcherInfo *watcherInfo = (WatcherInfo *) pData;
    std::string filepath(
            ABC_BridgeWatcherFile(watcherInfo->wallet.szUUID));

    std::ofstream file(filepath);
    if (!file.is_open())
    {
        ABC_DebugLog("Unable to open file for serialization");
    }
    else
    {
        db = watcherInfo->watcher->serialize();
        file.write(reinterpret_cast<const char *>(db.data()), db.size());
        file.close();
    }
    return NULL;
}

/**
 * Create a non-malleable tx id
 *
 * @param tx    The transaction to hash in a non-malleable way
 */
static std::string ABC_BridgeNonMalleableTxId(bc::transaction_type tx)
{
    for (auto& input: tx.inputs)
        input.script = bc::script_type();
    return bc::encode_hex(bc::hash_transaction(tx, bc::sighash::all));
}

Status
watcherBridgeRawTx(const char *szWalletUUID, const char *szTxID,
    DataChunk &result)
{
    auto row = watchers_.find(szWalletUUID);
    if (row == watchers_.end())
        return ABC_ERROR(ABC_CC_Synchronizing, "Unable to find watcher");
    WatcherInfo *watcherInfo = row->second;

    auto tx = watcherInfo->watcher->find_tx(bc::decode_hash(szTxID));
    result.resize(satoshi_raw_size(tx));
    bc::satoshi_save(tx, result.begin());

    return Status();
}

} // namespace abcd
