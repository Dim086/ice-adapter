#include "JsonRpcServer.h"

#include <webrtc/base/thread.h>

#include "logging.h"

namespace faf {

JsonRpcServer::JsonRpcServer():
  _server(rtc::Thread::Current()->socketserver()->CreateAsyncSocket(SOCK_STREAM)),
  _currentId(0)
{
}

JsonRpcServer::~JsonRpcServer()
{
  FAF_LOG_DEBUG << "~JsonRpcServer()";
}

void JsonRpcServer::listen(int port)
{
  _server->SignalReadEvent.connect(this, &JsonRpcServer::_onNewClient);
  if (_server->Bind(rtc::SocketAddress("127.0.0.1", port)) != 0)
  {
    FAF_LOG_ERROR << "unable to bind to port " << port;
    std::exit(1);
  }
  _server->Listen(5);
  FAF_LOG_INFO << "JsonRpcServer listening on port " << _server->GetLocalAddress().port();
}

int JsonRpcServer::listenPort() const
{
  return _server->GetLocalAddress().port();
}

void JsonRpcServer::setRpcCallback(std::string const& method,
                                   RpcCallback cb)
{
  _callbacks[method] = cb;
}

void JsonRpcServer::sendRequest(std::string const& method,
                                Json::Value const& paramsArray,
                                rtc::AsyncSocket* socket,
                                RpcRequestResult resultCb)
{
  if (!paramsArray.isArray())
  {
    Json::Value error = "paramsArray MUST be an array";
    if (resultCb)
    {
      resultCb(Json::Value(),
               error);
    }
    return;
  }
  if (method.empty())
  {
    Json::Value error = "method MUST not be empty";
    if (resultCb)
    {
      resultCb(Json::Value(),
               error);
    }
    return;
  }

  Json::Value request;
  request["jsonrpc"] = "2.0";
  request["method"] = method;
  request["params"] = paramsArray;
  if (resultCb)
  {
    _currentRequests[_currentId] = resultCb;
    request["id"] = _currentId;
    ++_currentId;
  }
  std::string requestString = Json::FastWriter().write(request);

  if (!_sendJson(requestString, socket))
  {
    Json::Value error = "send failed";
    if (resultCb)
    {
      resultCb(Json::Value(),
               error);
    }
  }
}

void JsonRpcServer::_onNewClient(rtc::AsyncSocket* socket)
{
  rtc::SocketAddress accept_addr;
  auto newConnectedSocket = std::shared_ptr<rtc::AsyncSocket>(_server->Accept(&accept_addr));
  newConnectedSocket->SignalReadEvent.connect(this, &JsonRpcServer::_onRead);
  newConnectedSocket->SignalCloseEvent.connect(this, &JsonRpcServer::_onClientDisconnect);
  _connectedSockets.insert(std::make_pair(newConnectedSocket.get(), newConnectedSocket));
  FAF_LOG_DEBUG << "JsonRpcServer client connected from " << accept_addr;
  SignalClientConnected.emit();
}

void JsonRpcServer::_onClientDisconnect(rtc::AsyncSocket* socket, int _whatsThis_)
{
  _currentMsgs.erase(socket);
  _connectedSockets.erase(socket);
  FAF_LOG_DEBUG << "JsonRpcServer client disonnected: " << _whatsThis_;
  SignalClientDisconnected.emit();
}

std::string trim_whitespace(std::string const& in)
{
  const std::string whitespace = " \t\f\v\n\r";
  auto start = in.find_first_not_of(whitespace);
  auto end = in.find_last_not_of(whitespace);
  if (start == std::string::npos ||
      end == std::string::npos)
  {
    return std::string();
  }
  return in.substr(start, end + 1);
}

void JsonRpcServer::_onRead(rtc::AsyncSocket* socket)
{
  if (_connectedSockets.find(socket) != _connectedSockets.end())
  {
    int msgLength = 0;
    std::string& msgBuffer = _currentMsgs[socket];
    do
    {
      msgLength = socket->Recv(_readBuffer.data(), _readBuffer.size(), nullptr);
      if (msgLength > 0)
      {
        msgBuffer.append(_readBuffer.data(), std::size_t(msgLength));
      }
    }
    while (msgLength > 0);
    while (true)
    {
      msgBuffer = trim_whitespace(msgBuffer);
      if (msgBuffer.empty())
      {
        break;
      }
      Json::Value json = _parseJsonFromMsgBuffer(msgBuffer);
      if (json.isNull())
      {
        break;
      }
      _processJsonMessage(json, socket);
    }
  }
}

Json::Value JsonRpcServer::_parseJsonFromMsgBuffer(std::string& msgBuffer)
{
  FAF_LOG_TRACE << "parsing JSON string: " << msgBuffer;
  Json::Value result;

  if (msgBuffer.empty())
  {
    return result;
  }
  if (msgBuffer.at(0) != '{')
  {
    msgBuffer.clear();
    FAF_LOG_ERROR << "invalid JSON msg";
    return result;
  }

  bool inString = false;
  int braceNestingLevel = 0;
  std::size_t msgPos = 0;

  for (; msgPos < msgBuffer.size(); ++msgPos)
  {
    const char& c = msgBuffer.at(msgPos);
    if (c == '"')
    {
      inString = !inString;
    }

    if (!inString)
    {
      if (c == '{')
      {
        ++braceNestingLevel;
      }

      if (c == '}')
      {
        --braceNestingLevel;
        if (braceNestingLevel < 0)
        {
          msgBuffer.clear();
          FAF_LOG_ERROR << "invalid JSON msg";
          return result;
        }

        /* parse msg */
        if (braceNestingLevel == 0)
        {
          Json::Reader reader;
          if (!reader.parse(std::string(msgBuffer.cbegin(),
                                        msgBuffer.cbegin() + static_cast<std::string::difference_type>(msgPos + 1)),
                            result))
          {
            FAF_LOG_ERROR << "error parsing JSON msg: " << reader.getFormatedErrorMessages();
            msgBuffer.clear();
            return result;
          }
          if (msgPos + 1 >= msgBuffer.size())
          {
            msgBuffer.clear();
          }
          else
          {
            msgBuffer = msgBuffer.substr(msgPos + 1);
          }
          return result;
        }
      }
    }
  }
  return result;
}

void JsonRpcServer::_processJsonMessage(Json::Value const& jsonMessage, rtc::AsyncSocket* socket)
{
  FAF_LOG_TRACE << "processing JSON msg: " << jsonMessage.toStyledString();
  if (jsonMessage.isMember("method"))
  {
    /* this message is a request */
    Json::Value response = _processRequest(jsonMessage, socket);

    /* we don't need to respond to notifications */
    if (jsonMessage.isMember("id"))
    {
      std::string responseString = Json::FastWriter().write(response);
      FAF_LOG_TRACE << "sending response:" << responseString;
      if (_connectedSockets.find(socket) != _connectedSockets.end())
      {
        socket->Send(responseString.c_str(), responseString.size());
      }
    }
  }
  else if (jsonMessage.isMember("error") ||
           jsonMessage.isMember("result"))
  {
    /* this message is a response */
    if (jsonMessage.isMember("id"))
    {
      if (jsonMessage["id"].isInt())
      {
        auto reqIt = _currentRequests.find(jsonMessage["id"].asInt());
        if (reqIt != _currentRequests.end())
        {
          try
          {
            reqIt->second(jsonMessage.isMember("result") ? jsonMessage["result"] : Json::Value(),
                          jsonMessage.isMember("error") ? jsonMessage["error"] : Json::Value());
          }
          catch (std::exception& e)
          {
            FAF_LOG_ERROR << "exception in request handler for id " << jsonMessage["id"].asInt() << ": " << e.what();
          }
          _currentRequests.erase(reqIt);
        }
      }
    }
  }
}

Json::Value JsonRpcServer::_processRequest(Json::Value const& request, rtc::AsyncSocket* socket)
{
  Json::Value response;
  response["jsonrpc"] = "2.0";

  if (request.isMember("id"))
  {
    response["id"] = request["id"];
  }
  if (!request.isMember("method"))
  {
    response["error"]["code"] = -1;
    response["error"]["message"] = "missing 'method' parameter";
    return response;
  }
  if (!request["method"].isString())
  {
    response["error"]["code"] = -1;
    response["error"]["message"] = "'method' parameter must be a string";
    return response;
  }

  FAF_LOG_TRACE << "dispatching JSRONRPC method '" << request["method"].asString() << "'";

  Json::Value params(Json::arrayValue);
  if (request.isMember("params") &&
      request["params"].isArray())
  {
    params = request["params"];
  }

  Json::Value result;
  Json::Value error;

  auto it = _callbacks.find(request["method"].asString());
  if (it != _callbacks.end())
  {
    try
    {
      it->second(params, result, error, socket);
    }
    catch (std::exception& e)
    {
      FAF_LOG_ERROR << "exception in callback for method '" << request["method"].asString() << "': " << e.what();
    }
  }
  else
  {
    FAF_LOG_ERROR << "RPC callback for method '" << request["method"].asString() << "' not found";
    error = std::string("RPC callback for method '") + request["method"].asString() + "' not found";
  }

  /* TODO: Better check for valid error/result combination */
  if (!result.isNull())
  {
    response["result"] = result;
  }
  else
  {
    response["error"] = error;
  }

  return response;
}

bool JsonRpcServer::_sendJson(std::string const& message, rtc::AsyncSocket* socket)
{
  if (_connectedSockets.empty())
  {
    FAF_LOG_ERROR << "mSessions.empty()";
    return false;
  }
  for (auto it = _connectedSockets.begin(), end = _connectedSockets.end(); it != end; ++it)
  {
    if (socket)
    {
      if (it->second.get() != socket)
      {
        continue;
      }
    }
    FAF_LOG_TRACE << "sending " << message;

    if (!it->second->Send(message.c_str(), message.size()))
    {
      _currentMsgs.erase(it->second.get());
      it = _connectedSockets.erase(it);
      FAF_LOG_ERROR << "sending " << message << " failed";
    }
    else
    {
      FAF_LOG_TRACE << " done";
    }
  }
  return true;
}

} // namespace faf
