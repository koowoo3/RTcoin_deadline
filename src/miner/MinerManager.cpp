// Copyright (c) 2012-2017, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2014-2018, The Monero Project
// Copyright (c) 2018-2020, The TurtleCoin Developers
//
// Please see the included LICENSE file for more information.

/////////////////////////
#include "MinerManager.h"
/////////////////////////

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <common/CryptoNoteTools.h>
#include <common/StringTools.h>
#include <common/TransactionExtra.h>
#include <config/CryptoNoteConfig.h>
#include <miner/BlockUtilities.h>
#include <utilities/ColouredMsg.h>
#include <utilities/FormatTools.h>

namespace Miner
{
    namespace
    {
        MinerEvent BlockMinedEvent()
        {
            MinerEvent event;
            event.type = MinerEventType::BLOCK_MINED;
            return event;
        }

        MinerEvent BlockchainUpdatedEvent()
        {
            MinerEvent event;
            event.type = MinerEventType::BLOCKCHAIN_UPDATED;
            return event;
        }

        void adjustMergeMiningTag(CryptoNote::BlockTemplate &blockTemplate)
        {
            if (blockTemplate.majorVersion >= CryptoNote::BLOCK_MAJOR_VERSION_2)
            {
                CryptoNote::TransactionExtraMergeMiningTag mmTag;
                mmTag.depth = 0;
                //merkle root란 block에 저장된 트랜잭션들의 hash tree
                //block body의 transaction들이 유효한지 빠르게 검사하는 역할
                mmTag.merkleRoot = getMerkleRoot(blockTemplate);

                blockTemplate.parentBlock.baseTransaction.extra.clear();
                if (!CryptoNote::appendMergeMiningTagToExtra(blockTemplate.parentBlock.baseTransaction.extra, mmTag))
                {
                    throw std::runtime_error("Couldn't append merge mining tag");
                }
            }
        }

    } // namespace

    MinerManager::MinerManager(
        System::Dispatcher &dispatcher,
        const CryptoNote::MiningConfig &config,
        const std::shared_ptr<httplib::Client> httpClient):

        m_contextGroup(dispatcher),
        m_config(config),
        m_miner(dispatcher),
        m_blockchainMonitor(dispatcher, m_config.scanPeriod, httpClient),
        m_eventOccurred(dispatcher),
        m_lastBlockTimestamp(0),
        m_httpClient(httpClient)
    {
    }

    void MinerManager::start()
    {
        //어떤 내용을 block으로 만들지 가져온다.
        CryptoNote::BlockMiningParameters params = requestMiningParameters();
        //block time을 넣어주는데, first block time이 0이면 skip
        adjustBlockTemplate(params.blockTemplate);

        isRunning = true;

        //다른 사람이 mining 했는지 확인하는 monitor. 새로운 block 발견하면 update event 날림
        startBlockchainMonitoring();
        std::thread reporter(std::bind(&MinerManager ::printHashRate, this));
        // 내가 mining start
        startMining(params);

        eventLoop();
        isRunning = false;
    }

    void MinerManager::printHashRate()
    {
        uint64_t last_hash_count = m_miner.getHashCount();

        while (isRunning)
        {
            std::this_thread::sleep_for(std::chrono::seconds(60));

            uint64_t current_hash_count = m_miner.getHashCount();

            double hashes = static_cast<double>((current_hash_count - last_hash_count) / 60);

            last_hash_count = current_hash_count;

            std::cout << SuccessMsg("\nMining at ") << SuccessMsg(Utilities::get_mining_speed(hashes)) << "\n\n";
        }
    }

    void MinerManager::eventLoop()
    {
        size_t blocksMined = 0;

        while (true)
        {
            MinerEvent event = waitEvent();

            switch (event.type)
            {
                case MinerEventType::BLOCK_MINED:
                //내가 mine 성공함 
                {
                    stopBlockchainMonitoring();
                    
                    //mine한 block 제출
                    if (submitBlock(m_minedBlock))
                    {
                        m_lastBlockTimestamp = m_minedBlock.timestamp;

                        //./miner 켤때 limit 설정할 수 있음. 몇개만 채굴하고 끝내라고.
                        if (m_config.blocksLimit != 0 && ++blocksMined == m_config.blocksLimit)
                        {
                            std::cout << InformationMsg("Mined requested amount of blocks (")
                                      << InformationMsg(m_config.blocksLimit) << InformationMsg("). Quitting.\n");
                            return;
                        }
                    }

                    CryptoNote::BlockMiningParameters params = requestMiningParameters();
                    adjustBlockTemplate(params.blockTemplate);

                    startBlockchainMonitoring();
                    startMining(params);
                    break;
                }
                case MinerEventType::BLOCKCHAIN_UPDATED:
                //나 말고 다른 사람이 mining 함. 다시 parent block을 받아서 mining 시작
                {
                    stopMining();
                    stopBlockchainMonitoring();
                    CryptoNote::BlockMiningParameters params = requestMiningParameters();
                    adjustBlockTemplate(params.blockTemplate);
                    startBlockchainMonitoring();
                    startMining(params);
                    break;
                }
            }
        }
    }

    MinerEvent MinerManager::waitEvent()
    {
        while (m_events.empty())
        {
            m_eventOccurred.wait();
            m_eventOccurred.clear();
        }

        MinerEvent event = std::move(m_events.front());
        m_events.pop();

        return event;
    }

    void MinerManager::pushEvent(MinerEvent &&event)
    {
        m_events.push(std::move(event));
        m_eventOccurred.set();
    }

    void MinerManager::startMining(const CryptoNote::BlockMiningParameters &params)
    {
        m_contextGroup.spawn(
            [this, params]()
            {
                try
                {
                    //mine, threadCount는 HW dependent. ./miner 호출할때 옵션으로 넣을 수 있음
                    m_minedBlock = m_miner.mine(params, m_config.threadCount);
                    //mine 완료 BLOCK_MINED 이벤트 보냄
                    pushEvent(BlockMinedEvent());
                }
                catch (const std::exception &)
                {
                }
            });
    }

    void MinerManager::stopMining()
    {
        m_miner.stop();
    }

    void MinerManager::startBlockchainMonitoring()
    {
        //dispatcher에 this() 함수 push.
        m_contextGroup.spawn(
            [this]()
            {
                try 
                {
                    m_blockchainMonitor.waitBlockchainUpdate();
                    //새로 block hash가 들어오면 BLOCKCHAIN_UPDATED 이벤트 날림
                    pushEvent(BlockchainUpdatedEvent());
                }
                catch (const std::exception &)
                {
                }
            });
    }

    void MinerManager::stopBlockchainMonitoring()
    {
        m_blockchainMonitor.stop();
    }

    bool MinerManager::submitBlock(const CryptoNote::BlockTemplate &minedBlock)
    {
        rapidjson::StringBuffer sb;

        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

        writer.String(Common::toHex(toBinaryArray(minedBlock)));

        //block을 binary로 만들어서 peer 들에게 http로 전달 
        auto res = m_httpClient->Post("/block", sb.GetString(), "application/json");

        // 202는 http 응답 상태 코드에서 Accepted 의미
        if (res && res->status == 202)
        {
            std::cout << SuccessMsg("\nBlock found! Hash: ") << SuccessMsg(getBlockHash(minedBlock)) << "\n\n";

            return true;
        }
        else
        {
            std::cout << WarningMsg("Failed to submit block, possibly daemon offline or syncing?\n");
            return false;
        }
    }

    CryptoNote::BlockMiningParameters MinerManager::requestMiningParameters()
    {
        while (true)
        {
            rapidjson::StringBuffer sb;

            rapidjson::Writer<rapidjson::StringBuffer> writer(sb);

            writer.StartObject();
            {
                writer.Key("address");
                writer.String(m_config.miningAddress);

                writer.Key("reserveSize");
                writer.Uint(0);
            }
            writer.EndObject();

            //http 서버에서 block template를 받아옴.
            auto res = m_httpClient->Post("/block/template", sb.GetString(), "application/json");

            if (!res)
            {
                std::cout << WarningMsg("Failed to get block template - Is your daemon open?\n");

                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            if (res->status != 201)
            {
                std::stringstream stream;

                stream << "Failed to get block template - received unexpected http "
                       << "code from server: " << res->status << std::endl;

                std::cout << WarningMsg(stream.str()) << std::endl;

                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            rapidjson::Document jsonBody;

            if (jsonBody.Parse(res->body.c_str()).HasParseError())
            {
                std::stringstream stream;

                stream << "Failed to parse block template from daemon. Received data:\n" << res->body << std::endl;

                std::cout << WarningMsg(stream.str());

                std::this_thread::sleep_for(std::chrono::seconds(1));

                continue;
            }

            CryptoNote::BlockMiningParameters params;

            //서버에서 받은 response에서 difficulty 읽어옴. 
            params.difficulty = getUint64FromJSON(jsonBody, "difficulty");
            
            //서버에서 받은 response에서 blob(binary large object) 읽어옴
            std::vector<uint8_t> blob = Common::fromHex(getStringFromJSON(jsonBody, "blob"));

            if (!fromBinaryArray(params.blockTemplate, blob))
            {
                std::cout << WarningMsg("Couldn't parse block template from daemon.") << std::endl;

                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            return params;
        }
    }

    void MinerManager::adjustBlockTemplate(CryptoNote::BlockTemplate &blockTemplate) const
    {
        adjustMergeMiningTag(blockTemplate);

        if (m_config.firstBlockTimestamp == 0)
        {
            /* no need to fix timestamp */
            return;
        }

        if (m_lastBlockTimestamp == 0)
        {
            blockTemplate.timestamp = m_config.firstBlockTimestamp;
        }
        else if (m_lastBlockTimestamp != 0 && m_config.blockTimestampInterval != 0)
        {
            blockTemplate.timestamp = m_lastBlockTimestamp + m_config.blockTimestampInterval;
        }
    }

} // namespace Miner
