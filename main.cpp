﻿#include <dpp/dpp.h>
std::unique_ptr<dpp::cluster> bot = std::make_unique<dpp::cluster>("MTAwNDUxNDkzNTA1OTAwNTQ3MA.______.", dpp::i_all_intents);
std::unique_ptr<std::unordered_map<dpp::snowflake, std::future<void>>> cmd_sender = std::make_unique<std::unordered_map<dpp::snowflake, std::future<void>>>();
std::unique_ptr<std::unordered_map<dpp::snowflake, std::future<void>>> btn_sender = std::make_unique<std::unordered_map<dpp::snowflake, std::future<void>>>();

struct giveaway {
	std::string title{}, description{};
	int64_t ends{}, winners{};
	dpp::snowflake host{};
	std::vector<dpp::snowflake> entries{};
	dpp::message message{};
	void message_update() {
		this->message.embeds[0]
			.set_description(std::format("{0} \n\nEnds: {1} \nHosted by: <@{2}> \nEntries: {3} \nWinners: {4}",
				this->description, dpp::utility::timestamp(this->ends, dpp::utility::tf_short_datetime), (uint64_t)this->host, this->entries.size(), this->winners));
		bot->message_edit(this->message);
	}
}; std::unique_ptr<std::unordered_map<size_t, giveaway>> _giveaway = std::make_unique<std::unordered_map<size_t, giveaway>>();

struct command_info {
	std::string name{}, description{};
	dpp::permissions permission{};
	std::vector<dpp::command_option> options{};
};

void button_pressed(std::unique_ptr<dpp::button_click_t> event) {
	std::unique_ptr<std::vector<std::string>> i = dpp::utility::index(event->custom_id, '.');
	if (i->at(0) == "giveaway") {
		std::unique_ptr<giveaway> gw = std::make_unique<giveaway>(_giveaway->at(stoull(i->at(1))));
		if (std::ranges::find(gw->entries, event->command.member.user_id) not_eq gw->entries.end())
			event->reply(dpp::message("> You have already entered this giveaway!")
				.set_flags(dpp::message_flags::m_ephemeral));
		else gw->entries.emplace_back(event->command.member.user_id);
		gw->message_update();
		_giveaway->at(stoull(i->at(1))) = std::move(*gw);
		event->reply();
	}
	std::this_thread::sleep_for(std::chrono::seconds(1));
	btn_sender->erase(event->command.member.user_id);
}

void release_command(std::unique_ptr<dpp::slashcommand_t> event)
{
	if (event->command.get_command_name() == "purge") {
		event->reply(std::make_unique<dpp::message>("> *Deleting..*")
			->set_flags(dpp::m_ephemeral));
		bot->messages_get(event->command.channel_id, 0, event->command.id, 0, std::clamp((int)get<int64_t>(event->get_parameter("amount")), 1, 100),
			[&event](const dpp::confirmation_callback_t& mg_cb)
			{
				bot->message_delete_bulk(std::move(std::get<dpp::message_map>(mg_cb.value)), event->command.channel_id,
				[&event, mg_cb](const dpp::confirmation_callback_t& mdb_cb)
					{
						std::string what = mdb_cb.is_error() ?
							std::format("> {0}", mdb_cb.get_error().message) :
							std::format("> Deleted **{0}** messages", std::get<dpp::message_map>(mg_cb.value).size());
						event->edit_original_response(std::make_unique<dpp::message>(std::move(what))
							->set_flags(dpp::message_flags::m_ephemeral));
					});
			});
	}
	if (event->command.get_command_name() == "gcreate") {
		giveaway gw = {
			get<std::string>(event->get_parameter("title")), get<std::string>(event->get_parameter("description")),
			get<int64_t>(event->get_parameter("time")), get<int64_t>(event->get_parameter("winners")),
			event->command.member.user_id, {}
		};
		bot->message_create(dpp::message(event->command.channel_id,
			std::make_unique<dpp::embed>()
			->set_color(dpp::colors::outter)
			.set_title(std::format("{0}", gw.title))
			.set_description(std::format("{0} \n\nEnds: {1} \nHosted by: <@{2}> \nEntries: {3} \nWinners: {4}",
				gw.description, dpp::utility::timestamp(gw.ends, dpp::utility::tf_short_datetime), (uint64_t)gw.host, gw.entries.size(), gw.winners)))
			.add_component(std::make_unique<dpp::component>()->add_component(std::make_unique<dpp::component>()
				->set_emoji(u8"🎉")
				.set_id(std::format(".giveaway.{0}",
					_giveaway->size())))),
			[&event, &gw](const dpp::confirmation_callback_t& mc_cb)
			{
				std::unique_ptr<dpp::message> msg = std::make_unique<dpp::message>(std::get<dpp::message>(mc_cb.value));
				gw.message = std::move(*msg);
				gw.description = std::move(get<std::string>(event->get_parameter("description"))); // TODO
				_giveaway->emplace(_giveaway->size(), gw);
			});
		event->reply(std::make_unique<dpp::message>(std::format("> The giveaway was successfully created! ID: **{0}**", _giveaway->size()))
			->set_flags(dpp::m_ephemeral));
	}
	std::this_thread::sleep_for(std::chrono::seconds(1));
	cmd_sender->erase(event->command.member.user_id);
}

int main()
{
	bot->on_ready([](const dpp::ready_t& event) {
		std::unique_ptr<std::vector<dpp::slashcommand>> cmds = std::make_unique<std::vector<dpp::slashcommand>>();
		std::unique_ptr<std::vector<command_info>> _command_info = std::make_unique<std::vector<command_info>>(std::vector<command_info>{
			{
				"purge", "mass delete messages", dpp::p_manage_messages,
				{
					{dpp::command_option(dpp::co_integer, "amount", "amount of messages to delete", true)}
				}
			},
			{
				"gcreate", "create a giveaway", dpp::p_administrator,
				{
					{dpp::command_option(dpp::co_string, "title", "what you're giveawaying", true)},
					{dpp::command_option(dpp::co_string, "description", "describe the giveaway", true)},
					{dpp::command_option(dpp::co_integer, "time", "the length of the giveaway", true)}, // -> improve
					{dpp::command_option(dpp::co_integer, "winners", "amount of winners", true)}
				}
			}});
		for (auto& [name, description, permission, options] : std::move(*_command_info)) {
			dpp::slashcommand cmd_handler;
			cmd_handler.set_name(name).set_description(description).set_default_permissions(permission);
			for (const auto& option : options) cmd_handler.add_option(option);
			cmds->emplace_back(cmd_handler);
		}
		bot->global_bulk_command_create(std::move(*cmds));
		});
	bot->on_slashcommand([](const dpp::slashcommand_t& event) {
		std::unique_ptr<dpp::slashcommand_t> _event = std::make_unique<dpp::slashcommand_t>(std::move(event));
		if (cmd_sender->contains(event.command.member.user_id)) return;
		cmd_sender->try_emplace(event.command.member.user_id, std::async(std::launch::async, release_command, std::move(_event)));
		});
	bot->on_button_click([](const dpp::button_click_t& event) {
		std::unique_ptr<dpp::button_click_t> _event = std::make_unique<dpp::button_click_t>(std::move(event));
		if (btn_sender->contains(event.command.member.user_id)) return;
		btn_sender->try_emplace(event.command.member.user_id, std::async(std::launch::async, button_pressed, std::move(_event)));
		});
	bot->on_log(dpp::utility::cout_logger());
	bot->start(dpp::start_type::st_wait);
}