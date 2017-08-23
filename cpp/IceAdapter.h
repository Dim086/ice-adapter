#pragma once

#include <string>
#include <memory>
#include <queue>

#include <glibmm.h>

#include "IceAdapterOptions.h"

namespace Json
{
  class Value;
}

namespace faf
{

/* Forward declarations */
class JsonRpcServer;
enum class ConnectionState;
class GPGNetServer;
class TcpSession;
class GPGNetMessage;
class PeerRelay;
class IceAgent;
typedef std::shared_ptr<IceAgent> IceAgentPtr;

struct IceAdapterGameTask
{
  enum
  {
    JoinGame,
    HostGame,
    ConnectToPeer,
    DisconnectFromPeer
  } task;
  std::string hostMap;
  std::string remoteLogin;
  int remoteId;
};


/*! \brief The main controller class
 *
 *  IceAdapter opens the JsonRpcTcpServer to be controlled
 *  by the game client.
 *  Creates the GPGNetServer for communication with the game.
 *  Creates one Relay for every Peer.
 */
class IceAdapter
{
public:
  IceAdapter(IceAdapterOptions const& options,
             Glib::RefPtr<Glib::MainLoop> mainloop);

  /** \brief Sets the IceAdapter in hosting mode and tells the connected game to host the map once
   *         it reaches "Lobby" state
       \param map: Map to host
      */
  void hostGame(std::string const& map);

  /** \brief Sets the IceAdapter in join mode and connects to the hosted lobby
   *         The IceAdapter will implicitly create a Relay for the remote player.
   *         The IceAdapter will ask the client for an SDP record of the remote peer via "rpcNeedSdp" JSONRPC notification.
   *         The IceAdapter will send the client the SDP record via "rpcGatheredSdp" JSONRPC notification.
       \param remotePlayerLogin: Login name of the player hosting the Lobby
       \param remotePlayerId:    ID of the player hosting the Lobby
      */
  void joinGame(std::string const& remotePlayerLogin,
                int remotePlayerId);

  /** \brief Tell the game to connect to a remote player once it reached Lobby state
   *         The same internal procedures as in @joinGame happen:
   *         The IceAdapter will implicitly create a Relay for the remote player.
   *         The IceAdapter will ask the client for an SDP record of the remote peer via "rpcNeedSdp" JSONRPC notification.
   *         The IceAdapter will send the client the SDP record via "rpcGatheredSdp" JSONRPC notification.
       \param remotePlayerLogin: Login name of the player to connect to
       \param remotePlayerId:    ID of the player to connect to
      */
  void connectToPeer(std::string const& remotePlayerLogin,
                     int remotePlayerId);

  /** \brief Not sure yet
      */
  void reconnectToPeer(int remotePlayerId);

  /** \brief Tell the game to disconnect from a remote peer
   *         Will remove the Relay.
       \param remotePlayerId:    ID of the player to disconnect from
      */
  void disconnectFromPeer(int remotePlayerId);

  /** \brief Sets the SDP record for the remote peer.
   *         This method assumes a previous call of joinGame or connectToPeer.
       \param remotePlayerId: ID of the remote player
       \param sdp:            the SDP message
      */
  void addSdp(int remotePlayerId, std::string const& sdp);

  /** \brief Send an arbitrary GPGNet message to the game
       \param message: The GPGNet message
      */
  void sendToGpgNet(GPGNetMessage const& message);

  /** \brief Return the ICEAdapters status
       \returns The status as JSON structure
      */
  Json::Value status() const;

protected:
  void onGpgNetMessage(GPGNetMessage const& message);
  void onGpgConnectionStateChanged(TcpSession* session, ConnectionState cs);

  void connectRpcMethods();

  void queueGameTask(IceAdapterGameTask t);
  void tryExecuteGameTasks();

  std::shared_ptr<PeerRelay> createPeerRelay(int remotePlayerId,
                                             std::string const& remotePlayerLogin);

  IceAgentPtr mAgent;
  IceAdapterOptions mOptions;
  std::shared_ptr<JsonRpcServer> mRpcServer;
  std::shared_ptr<GPGNetServer> mGPGNetServer;
  Glib::RefPtr<Glib::MainLoop> mMainloop;

  std::string mGPGNetGameState;

  std::map<int, std::shared_ptr<PeerRelay>> mRelays;

  std::queue<IceAdapterGameTask> mGameTasks;
};

}
