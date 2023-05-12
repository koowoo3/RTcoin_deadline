#pragma once

#include "CryptoTypes.h"
#include "httplib.h"

#include <optional>
#include <system/ContextGroup.h>
#include <system/Dispatcher.h>
#include <system/Event.h>

class BlockchainChecker
{
  public:
    BlockchainChecker(
        System::Dispatcher &dispatcher,
        const size_t checkingInterval);

    void waitBlockchainCheckerExpired();

    void stop();

    bool getCheckerStatus();

  private:
    System::Dispatcher &m_dispatcher;

    size_t m_checkingInterval;

    bool m_stopped;

    System::ContextGroup m_sleepingContext;
};