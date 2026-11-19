#include "zc/websocket_pool.h"

#include <string>

int websocket_pool_connect(std::string, std::string& err)
{
    err = "Not Implemented";
    return -1;
}

void websocket_pool_send(int, WebSocketMessageType, const std::string&)
{
}

bool websocket_pool_has_message(int)
{
    return false;
}

WebSocketMessageType websocket_pool_message_type(int)
{
	return WebSocketMessageType::None;
}

std::pair<WebSocketMessageType, std::string> websocket_pool_receive(int)
{
    return {};
}

WebSocketStatus websocket_pool_status(int)
{
    return WebSocketStatus::Closed;
}

std::string websocket_pool_error(int)
{
    return "Not Implemented";
}

void websocket_pool_close(int)
{
}

void websocket_pool_destroy()
{
}
