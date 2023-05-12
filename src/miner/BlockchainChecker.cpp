#include "BlockchainChecker.h"

#include "common/StringTools.h"

#include <iostream>
#include <system/EventLock.h>
#include <system/InterruptedException.h>
#include <system/Timer.h>
#include <utilities/ColouredMsg.h>
#include <version.h>

BlockchainChecker::BlockchainChecker(
    System::Dispatcher &dispatcher,
    const size_t checkingInterval):

    m_dispatcher(dispatcher),
    m_checkingInterval(checkingInterval),
    m_stopped(false),
    m_sleepingContext(dispatcher)
{

}

void BlockchainChecker::waitBlockchainCheckerExpired()
{
    m_stopped = false;

    m_sleepingContext.spawn(
        [this]()
        {
            System::Timer timer(m_dispatcher);
            timer.sleep(std::chrono::seconds(m_checkingInterval));
        });

    m_sleepingContext.wait();
}

void BlockchainChecker::stop()
{
    m_stopped = true;

    m_sleepingContext.interrupt();
    m_sleepingContext.wait();
}

bool BlockchainChecker::getCheckerStatus()
{
    return m_stopped;
}
