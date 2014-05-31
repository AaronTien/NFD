/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2014  Regents of the University of California,
 *                     Arizona Board of Regents,
 *                     Colorado State University,
 *                     University Pierre & Marie Curie, Sorbonne University,
 *                     Washington University in St. Louis,
 *                     Beijing Institute of Technology
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/name.hpp>

#include <ndn-cxx/management/nfd-face-event-notification.hpp>
#include <ndn-cxx/management/nfd-controller.hpp>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>

#include "version.hpp"
#include "core/face-uri.hpp"
#include "network.hpp"

namespace po = boost::program_options;

namespace nfd {

using namespace ndn::nfd;
using ndn::Face;

class AutoregServer
{
public:
  AutoregServer()
    : m_controller(m_face)
    // , m_lastNotification(<undefined>)
    , m_cost(255)
  {
  }

  void
  onNfdCommandSuccess(const FaceEventNotification& notification)
  {
    std::cerr << "SUCCEED: " << notification << std::endl;
  }

  void
  onNfdCommandFailure(const FaceEventNotification& notification,
                      uint32_t code, const std::string& reason)
  {
    std::cerr << "FAILED: " << notification
              << " (code: " << code << ", reason: " << reason << ")" << std::endl;
  }

  bool
  isFiltered(const FaceUri& uri)
  {
    const std::string& scheme = uri.getScheme();
    if (!(scheme == "udp4" || scheme == "tcp4" ||
          scheme == "udp6" || scheme == "tcp6"))
      return true;

    boost::asio::ip::address address = boost::asio::ip::address::from_string(uri.getHost());

    for (std::vector<Network>::const_iterator network = m_blackList.begin();
         network != m_blackList.end();
         ++network)
      {
        if (network->doesContain(address))
          return true;
      }

    for (std::vector<Network>::const_iterator network = m_whiteList.begin();
         network != m_whiteList.end();
         ++network)
      {
        if (network->doesContain(address))
          return false;
      }

    return true;
  }

  void
  processCreateFace(const FaceEventNotification& notification)
  {
    FaceUri uri(notification.getRemoteUri());

    if (isFiltered(uri))
      {
        std::cerr << "Not processing (filtered): " << notification << std::endl;
        return;
      }

    for (std::vector<ndn::Name>::const_iterator prefix = m_autoregPrefixes.begin();
         prefix != m_autoregPrefixes.end();
         ++prefix)
      {
        std::cout << "Try auto-register: " << *prefix << std::endl;

        m_controller.start<FibAddNextHopCommand>(
          ControlParameters()
            .setName(*prefix)
            .setFaceId(notification.getFaceId())
            .setCost(m_cost),
          bind(&AutoregServer::onNfdCommandSuccess, this, notification),
          bind(&AutoregServer::onNfdCommandFailure, this, notification, _1, _2));
      }
  }

  void
  onNotification(const Data& data)
  {
    m_lastNotification = data.getName().get(-1).toSegment();

    // process
    FaceEventNotification notification(data.getContent().blockFromValue());

    if (notification.getKind() == FACE_EVENT_CREATED &&
        !notification.isLocal() &&
        notification.isOnDemand())
      {
        processCreateFace(notification);
      }
    else
      {
        std::cout << "IGNORED: " << notification << std::endl;
      }

    Name nextNotification("/localhost/nfd/faces/events");
    nextNotification
      .appendSegment(m_lastNotification + 1);

    // no need to set freshness or child selectors
    m_face.expressInterest(nextNotification,
                           bind(&AutoregServer::onNotification, this, _2),
                           bind(&AutoregServer::onTimeout, this, _1));
  }

  void
  onTimeout(const Interest& timedOutInterest)
  {
    // re-express the timed out interest, but reset Nonce, since it has to change

    // To be robust against missing notification, use ChildSelector and MustBeFresh
    Interest interest("/localhost/nfd/faces/events");
    interest
      .setMustBeFresh(true)
      .setChildSelector(1)
      .setInterestLifetime(time::seconds(60))
      ;

    m_face.expressInterest(interest,
                           bind(&AutoregServer::onNotification, this, _2),
                           bind(&AutoregServer::onTimeout, this, _1));
  }

  void
  signalHandler()
  {
    m_face.shutdown();
  }



  void
  usage(std::ostream& os,
        const po::options_description& optionDesciption,
        const char* programName)
  {
    os << "Usage:\n"
       << "  " << programName << " --prefix=</autoreg/prefix> [--prefix=/another/prefix] ...\n"
       << "\n";
    os << optionDesciption;
  }

  void
  startProcessing()
  {
    std::cout << "AUTOREG prefixes: " << std::endl;
    for (std::vector<ndn::Name>::const_iterator prefix = m_autoregPrefixes.begin();
         prefix != m_autoregPrefixes.end();
         ++prefix)
      {
        std::cout << "  " << *prefix << std::endl;
      }

    if (!m_blackList.empty())
      {
        std::cout << "Blacklisted networks: " << std::endl;
        for (std::vector<Network>::const_iterator network = m_blackList.begin();
             network != m_blackList.end();
             ++network)
          {
            std::cout << "  " << *network << std::endl;
          }
      }

    std::cout << "Whitelisted networks: " << std::endl;
    for (std::vector<Network>::const_iterator network = m_whiteList.begin();
         network != m_whiteList.end();
         ++network)
      {
        std::cout << "  " << *network << std::endl;
      }

    Interest interest("/localhost/nfd/faces/events");
    interest
      .setMustBeFresh(true)
      .setChildSelector(1)
      .setInterestLifetime(time::seconds(60))
      ;

    m_face.expressInterest(interest,
                           bind(&AutoregServer::onNotification, this, _2),
                           bind(&AutoregServer::onTimeout, this, _1));

    boost::asio::signal_set signalSet(m_face.getIoService(), SIGINT, SIGTERM);
    signalSet.async_wait(bind(&AutoregServer::signalHandler, this));

    m_face.processEvents();
  }

  int
  main(int argc, char* argv[])
  {
    po::options_description optionDesciption;
    optionDesciption.add_options()
      ("help,h", "produce help message")
      ("prefix,i", po::value<std::vector<ndn::Name> >(&m_autoregPrefixes)->composing(),
       "prefix that should be automatically registered when new remote face is established")
      ("cost,c", po::value<uint64_t>(&m_cost)->default_value(255),
       "FIB cost which should be assigned to autoreg nexthops")
      ("whitelist,w", po::value<std::vector<Network> >(&m_whiteList)->composing(),
       "Whitelisted network, e.g., 192.168.2.0/24 or ::1/128")
      ("blacklist,b", po::value<std::vector<Network> >(&m_blackList)->composing(),
       "Blacklisted network, e.g., 192.168.2.32/30 or ::1/128")
      ("version,V", "show version and exit")
      ;

    po::variables_map options;
    try
      {
        po::store(po::command_line_parser(argc, argv).options(optionDesciption).run(), options);
        po::notify(options);
      }
    catch (std::exception& e)
      {
        std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
        usage(std::cerr, optionDesciption, argv[0]);
        return 1;
      }

    if (options.count("help"))
      {
        usage(std::cout, optionDesciption, argv[0]);
        return 0;
      }

    if (options.count("version"))
      {
        std::cout << NFD_VERSION_BUILD_STRING << std::endl;
        return 0;
      }

    if (m_autoregPrefixes.empty())
      {
        std::cerr << "ERROR: at least one --prefix must be specified" << std::endl << std::endl;
        usage(std::cerr, optionDesciption, argv[0]);
        return 2;
      }

    if (m_whiteList.empty())
      {
        // Allow everything
        m_whiteList.push_back(Network::getMaxRangeV4());
        m_whiteList.push_back(Network::getMaxRangeV6());
      }

    try
      {
        startProcessing();
      }
    catch (std::exception& e)
      {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 2;
      }

    return 0;
  }

private:
  Face m_face;
  Controller m_controller;
  uint64_t m_lastNotification;
  std::vector<ndn::Name> m_autoregPrefixes;
  uint64_t m_cost;
  std::vector<Network> m_whiteList;
  std::vector<Network> m_blackList;
};

} // namespace nfd

int
main(int argc, char* argv[])
{
  nfd::AutoregServer server;
  return server.main(argc, argv);
}
