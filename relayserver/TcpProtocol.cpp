#include "TcpProtocol.h"
#include <stdint.h>
#include <unistd.h>
#include <array>
#include <msgpack.hpp>
#include <iostream>

void TcpProtocol::SetMessageReceivedCallback(TcpProtocol::MessageReceivedCallback callback)
{
	_messageReceivedCallback = callback;
}

bool TcpProtocol::Read(int socket)
{
	std::array<char, 1024> readbuf;
	ssize_t bytesRead = read(socket, readbuf.data(), readbuf.size());
	if (bytesRead<=0) { return false; }
	_buf.insert(_buf.end(), &readbuf[0], &readbuf[static_cast<size_t>(bytesRead)]);

	while (true)
	{
		if ((_awaitedSize==0) && (_buf.size() >= 4))
		{
			_awaitedSize = static_cast<uint8_t>(_buf[0])<<24
						 | static_cast<uint8_t>(_buf[1])<<16
						 | static_cast<uint8_t>(_buf[2])<<8
						 | static_cast<uint8_t>(_buf[3])<<0;
			_buf.erase(_buf.begin(), _buf.begin()+4);
		}
		else if ((_awaitedSize>0) && (_buf.size() >= _awaitedSize))
		{
			std::vector<char> v(_buf.begin(), _buf.begin()+_awaitedSize);
			OnMessageReceived(v);
			_buf.erase(_buf.begin(), _buf.begin()+_awaitedSize);
			_awaitedSize = 0;
		}
		else
		{
			return true;
		}
	}
}

void TcpProtocol::OnMessageReceived(std::vector<char> &data)
{
	_messageReceivedCallback(data);

	msgpack::unpacker pac;
	msgpack::object_handle obj;

	pac.reserve_buffer(data.size());
	memcpy(pac.buffer(), data.data(), data.size());
	pac.buffer_consumed(data.size());

	if (!pac.next(obj)) { return; }
	if (obj.get().type != msgpack::type::ARRAY) { return; }
	auto arr = obj.get().via.array;
	if (arr.size<2) { return; }

	uint64_t version = arr.ptr[0].via.u64;
	uint64_t message_type = arr.ptr[1].via.u64;

	switch (message_type)
	{
		case MsgPackProtocol::MESSAGE_TYPE_GAME_INFO:
			OnGameInfoReceived(obj.get().as<MsgPackProtocol::GameInfoMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_WORLD_UPDATE:
			OnWorldUpdateReceived(obj.get().as<MsgPackProtocol::WorldUpdateMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_TICK:
			std::cerr << "tick" << std::endl;
			break;

		case MsgPackProtocol::MESSAGE_TYPE_BOT_SPAWN:
			OnBotSpawnReceived(obj.get().as<MsgPackProtocol::BotSpawnMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_BOT_KILL:
			OnBotKillReceived(obj.get().as<MsgPackProtocol::BotKillMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_BOT_MOVE:
			OnBotMoveReceived(obj.get().as<MsgPackProtocol::BotMoveMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_FOOD_SPAWN:
			OnFoodSpawnReceived(obj.get().as<MsgPackProtocol::FoodSpawnMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_FOOD_CONSUME:
			OnFoodConsumedReceived(obj.get().as<MsgPackProtocol::FoodConsumeMessage>());
			break;

		case MsgPackProtocol::MESSAGE_TYPE_FOOD_DECAY:
			OnFoodDecayedReceived(obj.get().as<MsgPackProtocol::FoodDecayMessage>());
			break;
	}
}

void TcpProtocol::OnGameInfoReceived(const MsgPackProtocol::GameInfoMessage& msg)
{
	_segments = std::make_unique<SnakeSegmentMap>(msg.world_size_x, msg.world_size_y, 1000);
	_food = std::make_unique<FoodMap>(msg.world_size_x, msg.world_size_y, 1000);
	_gameInfo = msg;
}

void TcpProtocol::OnWorldUpdateReceived(const MsgPackProtocol::WorldUpdateMessage &msg)
{
	for (auto& bot: msg.bots)
	{
		_bots.push_back(bot);
	}

	if (_food == nullptr) { return; }
	for (auto& food: msg.food)
	{
		_food->addElement(food);
	}
}

void TcpProtocol::OnFoodSpawnReceived(const MsgPackProtocol::FoodSpawnMessage& msg)
{
	if (_food == nullptr) { return; }
	for (auto& item: msg.new_food)
	{
		_food->addElement(item);
	}
}

void TcpProtocol::OnFoodConsumedReceived(const MsgPackProtocol::FoodConsumeMessage &msg)
{
	if (_food == nullptr) { return; }
	for (auto& item: msg.items)
	{
		_food->erase_if([item](const FoodItem& food) { return food.guid == item.food_id; });
	}
}

void TcpProtocol::OnFoodDecayedReceived(const MsgPackProtocol::FoodDecayMessage &msg)
{
	if (_food == nullptr) { return; }
	for (auto& item: msg.food_ids)
	{
		_food->erase_if([item](const FoodItem& food) { return food.guid == item; });
	}
}

void TcpProtocol::OnBotSpawnReceived(const MsgPackProtocol::BotSpawnMessage &msg)
{
	_bots.push_back(msg.bot);
}

void TcpProtocol::OnBotKillReceived(const MsgPackProtocol::BotKillMessage& msg)
{
	_bots.erase(std::remove_if(_bots.begin(), _bots.end(), [msg](const BotItem& bot) { return bot.guid == msg.victim_id; }));
}

void TcpProtocol::OnBotMoveReceived(const MsgPackProtocol::BotMoveMessage &msg)
{
	for (auto& item: msg.items)
	{
		auto it = std::find_if(_bots.begin(), _bots.end(), [item](const BotItem& bot) { return bot.guid == item.bot_id; });
		if (it == _bots.end()) { return; }
		auto& bot = *it;

		bot.segments.insert(bot.segments.begin(), item.new_segments.begin(), item.new_segments.end());
		bot.segments.resize(item.current_length);
		bot.segment_radius = item.current_segment_radius;
	}
}

