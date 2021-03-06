/*

Copyright (c) 2006, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/pch.hpp"

#include <utility>
#include <boost/bind.hpp>
#include <boost/function/function1.hpp>

#include "libtorrent/io.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/alert.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/random.hpp"
#include "libtorrent/aux_/session_impl.hpp"
#include "libtorrent/kademlia/node_id.hpp"
#include "libtorrent/kademlia/rpc_manager.hpp"
#include "libtorrent/kademlia/routing_table.hpp"
#include "libtorrent/kademlia/node.hpp"

#include "libtorrent/kademlia/refresh.hpp"
#include "libtorrent/kademlia/find_data.hpp"

namespace libtorrent { namespace dht
{

void incoming_error(entry& e, char const* msg);

using detail::write_endpoint;

int search_torrent_entry::match(char const* in_tags[], int num_tags) const
{
	int ret = 0;
	for (int i = 0; i < num_tags; ++i)
	{
		char const* t = in_tags[i];
		std::map<std::string, int>::const_iterator j = tags.find(t);
		if (j == tags.end()) continue;
		// weigh the score by how popular this tag is in this torrent
		ret += 100 * j->second / total_tag_points;
	}
	return ret;
}

bool search_torrent_entry::tick()
{
	int sum = 0;
	for (std::map<std::string, int>::iterator i = tags.begin()
		, end(tags.end()); i != end;)
	{
		i->second = (i->second * 2) / 3;
		sum += i->second;
		if (i->second > 0) { ++i; continue; }
		tags.erase(i++);
	}
	total_tag_points = sum;

	sum = 0;
	for (std::map<std::string, int>::iterator i = name.begin()
		, end(name.end()); i != end;)
	{
		i->second = (i->second * 2) / 3;
		sum += i->second;
		if (i->second > 0) { ++i; continue; }
		name.erase(i++);
	}
	total_name_points = sum;

	return total_tag_points == 0;
}

void search_torrent_entry::publish(std::string const& torrent_name, char const* in_tags[]
	, int num_tags)
{
	for (int i = 0; i < num_tags; ++i)
	{
		char const* t = in_tags[i];
		std::map<std::string, int>::iterator j = tags.find(t);
		if (j != tags.end())
			++j->second;
		else
			tags[t] = 1;
		++total_tag_points;
		// TODO: limit the number of tags
	}

	name[torrent_name] += 1;
	++total_name_points;

	// TODO: limit the number of names
}

void search_torrent_entry::get_name(std::string& t) const
{
	std::map<std::string, int>::const_iterator max = name.begin();
	for (std::map<std::string, int>::const_iterator i = name.begin()
		, end(name.end()); i != end; ++i)
	{
		if (i->second > max->second) max = i;
	}
	t = max->first;
}

void search_torrent_entry::get_tags(std::string& t) const
{
	for (std::map<std::string, int>::const_iterator i = tags.begin()
		, end(tags.end()); i != end; ++i)
	{
		if (i != tags.begin()) t += " ";
		t += i->first;
	}
}

#ifdef _MSC_VER
namespace
{
	char rand() { return (char)std::rand(); }
}
#endif

// TODO: configurable?
enum { announce_interval = 30 };

#ifdef TORRENT_DHT_VERBOSE_LOGGING
TORRENT_DEFINE_LOG(node)
#endif

// remove peers that have timed out
void purge_peers(std::set<peer_entry>& peers)
{
	for (std::set<peer_entry>::iterator i = peers.begin()
		  , end(peers.end()); i != end;)
	{
		// the peer has timed out
		if (i->added + minutes(int(announce_interval * 1.5f)) < time_now())
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			TORRENT_LOG(node) << "peer timed out at: " << i->addr;
#endif
			peers.erase(i++);
		}
		else
			++i;
	}
}

void nop() {}

node_impl::node_impl(libtorrent::alert_manager& alerts
	, bool (*f)(void*, entry&, udp::endpoint const&, int)
	, dht_settings const& settings, node_id nid, address const& external_address
	, external_ip_fun ext_ip, void* userdata)
	: m_settings(settings)
	, m_id(nid == (node_id::min)() || !verify_id(nid, external_address) ? generate_id(external_address) : nid)
	, m_table(m_id, 8, settings)
	, m_rpc(m_id, m_table, f, userdata, ext_ip)
	, m_last_tracker_tick(time_now())
	, m_alerts(alerts)
	, m_send(f)
	, m_userdata(userdata)
{
	m_secret[0] = random();
	m_secret[1] = std::rand();
}

bool node_impl::verify_token(std::string const& token, char const* info_hash
	, udp::endpoint const& addr)
{
	if (token.length() != 4)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		TORRENT_LOG(node) << "token of incorrect length: " << token.length();
#endif
		return false;
	}

	hasher h1;
	error_code ec;
	std::string address = addr.address().to_string(ec);
	if (ec) return false;
	h1.update(&address[0], address.length());
	h1.update((char*)&m_secret[0], sizeof(m_secret[0]));
	h1.update((char*)info_hash, sha1_hash::size);
	
	sha1_hash h = h1.final();
	if (std::equal(token.begin(), token.end(), (signed char*)&h[0]))
		return true;
		
	hasher h2;
	h2.update(&address[0], address.length());
	h2.update((char*)&m_secret[1], sizeof(m_secret[1]));
	h2.update((char*)info_hash, sha1_hash::size);
	h = h2.final();
	if (std::equal(token.begin(), token.end(), (signed char*)&h[0]))
		return true;
	return false;
}

std::string node_impl::generate_token(udp::endpoint const& addr, char const* info_hash)
{
	std::string token;
	token.resize(4);
	hasher h;
	error_code ec;
	std::string address = addr.address().to_string(ec);
	TORRENT_ASSERT(!ec);
	h.update(&address[0], address.length());
	h.update((char*)&m_secret[0], sizeof(m_secret[0]));
	h.update(info_hash, sha1_hash::size);

	sha1_hash hash = h.final();
	std::copy(hash.begin(), hash.begin() + 4, (signed char*)&token[0]);
	return token;
}

void node_impl::refresh(node_id const& id
	, find_data::nodes_callback const& f)
{
	boost::intrusive_ptr<dht::refresh> r(new dht::refresh(*this, id, f));
	r->start();
}

void node_impl::bootstrap(std::vector<udp::endpoint> const& nodes
	, find_data::nodes_callback const& f)
{
	boost::intrusive_ptr<dht::refresh> r(new dht::bootstrap(*this, m_id, f));

#ifdef TORRENT_DHT_VERBOSE_LOGGING
	int count = 0;
#endif

	for (std::vector<udp::endpoint>::const_iterator i = nodes.begin()
		, end(nodes.end()); i != end; ++i)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		++count;
#endif
		r->add_entry(node_id(0), *i, observer::flag_initial);
	}
	
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(node) << "bootstrapping with " << count << " nodes";
#endif
	r->start();
}

int node_impl::bucket_size(int bucket)
{
	return m_table.bucket_size(bucket);
}

void node_impl::new_write_key()
{
	m_secret[1] = m_secret[0];
	m_secret[0] = std::rand();
}

void node_impl::unreachable(udp::endpoint const& ep)
{
	m_rpc.unreachable(ep);
}

void node_impl::incoming(msg const& m)
{
	// is this a reply?
	lazy_entry const* y_ent = m.message.dict_find_string("y");
	if (!y_ent || y_ent->string_length() == 0)
	{
		entry e;
		incoming_error(e, "missing 'y' entry");
		m_send(m_userdata, e, m.addr, 0);
		return;
	}

	char y = *(y_ent->string_ptr());

	switch (y)
	{
		case 'r':
		{
			node_id id;
			if (m_rpc.incoming(m, &id))
				refresh(id, boost::bind(&nop));
			break;
		}
		case 'q':
		{
			TORRENT_ASSERT(m.message.dict_find_string_value("y") == "q");
			entry e;
			incoming_request(m, e);
			m_send(m_userdata, e, m.addr, 0);
			break;
		}
		case 'e':
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			lazy_entry const* err = m.message.dict_find_list("e");
			if (err && err->list_size() >= 2)
			{
				TORRENT_LOG(node) << "INCOMING ERROR: " << err->list_string_value_at(1);
			}
#endif
			break;
		}
	}
}

namespace
{
	void announce_fun(std::vector<std::pair<node_entry, std::string> > const& v
		, node_impl& node, int listen_port, sha1_hash const& ih)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		TORRENT_LOG(node) << "sending announce_peer [ ih: " << ih
			<< " p: " << listen_port
			<< " nodes: " << v.size() << " ]" ;
#endif

		// create a dummy traversal_algorithm		
		boost::intrusive_ptr<traversal_algorithm> algo(
			new traversal_algorithm(node, (node_id::min)()));

		// store on the first k nodes
		for (std::vector<std::pair<node_entry, std::string> >::const_iterator i = v.begin()
			, end(v.end()); i != end; ++i)
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			TORRENT_LOG(node) << "  distance: " << (160 - distance_exp(ih, i->first.id));
#endif

			void* ptr = node.m_rpc.allocate_observer();
			if (ptr == 0) return;
			observer_ptr o(new (ptr) announce_observer(algo, i->first.ep(), i->first.id));
#ifdef TORRENT_DEBUG
			o->m_in_constructor = false;
#endif
			entry e;
			e["y"] = "q";
			e["q"] = "announce_peer";
			entry& a = e["a"];
			a["info_hash"] = ih.to_string();
			a["port"] = listen_port;
			a["token"] = i->second;
			node.m_rpc.invoke(e, i->first.ep(), o);
		}
	}
}

void node_impl::add_router_node(udp::endpoint router)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(node) << "adding router node: " << router;
#endif
	m_table.add_router_node(router);
}

void node_impl::add_node(udp::endpoint node)
{
	// ping the node, and if we get a reply, it
	// will be added to the routing table
	void* ptr = m_rpc.allocate_observer();
	if (ptr == 0) return;

	// create a dummy traversal_algorithm		
	// this is unfortunately necessary for the observer
	// to free itself from the pool when it's being released
	boost::intrusive_ptr<traversal_algorithm> algo(
		new traversal_algorithm(*this, (node_id::min)()));
	observer_ptr o(new (ptr) null_observer(algo, node, node_id(0)));
#ifdef TORRENT_DEBUG
	o->m_in_constructor = false;
#endif
	entry e;
	e["y"] = "q";
	e["q"] = "ping";
	m_rpc.invoke(e, node, o);
}

void node_impl::announce(sha1_hash const& info_hash, int listen_port
	, boost::function<void(std::vector<tcp::endpoint> const&)> f)
{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
	TORRENT_LOG(node) << "announcing [ ih: " << info_hash << " p: " << listen_port << " ]" ;
#endif
	// search for nodes with ids close to id or with peers
	// for info-hash id. then send announce_peer to them.
	boost::intrusive_ptr<find_data> ta(new find_data(*this, info_hash, f
		, boost::bind(&announce_fun, _1, boost::ref(*this)
		, listen_port, info_hash)));
	ta->start();
}

void node_impl::tick()
{
	node_id target;
	if (m_table.need_refresh(target))
		refresh(target, boost::bind(&nop));
}

time_duration node_impl::connection_timeout()
{
	time_duration d = m_rpc.tick();
	ptime now(time_now());
	if (now - m_last_tracker_tick < minutes(2)) return d;
	m_last_tracker_tick = now;

	for (feed_table_t::iterator i = m_feeds.begin(); i != m_feeds.end();)
	{
		if (i->second.last_seen + minutes(60) > now)
		{
			++i;
			continue;
		}
		m_feeds.erase(i++);
	}

	// look through all peers and see if any have timed out
	for (table_t::iterator i = m_map.begin(), end(m_map.end()); i != end;)
	{
		torrent_entry& t = i->second;
		node_id const& key = i->first;
		++i;
		purge_peers(t.peers);

		// if there are no more peers, remove the entry altogether
		if (t.peers.empty())
		{
			table_t::iterator i = m_map.find(key);
			if (i != m_map.end()) m_map.erase(i);
		}
	}

	return d;
}

void node_impl::status(session_status& s)
{
	mutex_t::scoped_lock l(m_mutex);

	m_table.status(s);
	s.dht_torrents = int(m_map.size());
	s.active_requests.clear();
	s.dht_total_allocations = m_rpc.num_allocated_observers();
	for (std::set<traversal_algorithm*>::iterator i = m_running_requests.begin()
		, end(m_running_requests.end()); i != end; ++i)
	{
		s.active_requests.push_back(dht_lookup());
		dht_lookup& l = s.active_requests.back();
		(*i)->status(l);
	}
}

bool node_impl::lookup_torrents(sha1_hash const& target
	, entry& reply, char* tags) const
{
//	if (m_alerts.should_post<dht_find_torrents_alert>())
//		m_alerts.post_alert(dht_find_torrents_alert(info_hash));

	search_table_t::const_iterator first, last;
	first = m_search_map.lower_bound(std::make_pair(target, (sha1_hash::min)()));
	last = m_search_map.upper_bound(std::make_pair(target, (sha1_hash::max)()));

	if (first == last) return false;

	std::string tags_copy(tags);
	char const* in_tags[20];
	int num_tags = 0;
	num_tags = split_string(in_tags, 20, &tags_copy[0]);

	typedef std::pair<int, search_table_t::const_iterator> sort_item;
	std::vector<sort_item> result;
	for (; first != last; ++first)
	{
		result.push_back(std::make_pair(
			first->second.match(in_tags, num_tags), first));
	}

	std::sort(result.begin(), result.end()
		, boost::bind(&sort_item::first, _1) > boost::bind(&sort_item::first, _2));
	int num = (std::min)((int)result.size(), m_settings.max_torrent_search_reply);

	entry::list_type& pe = reply["values"].list();
	for (int i = 0; i < num; ++i)
	{
		pe.push_back(entry());
		entry::list_type& e = pe.back().list();
		// push name
		e.push_back(entry());
		result[i].second->second.get_name(e.back().string());
		// push tags
		e.push_back(entry());
		result[i].second->second.get_tags(e.back().string());
		// push info-hash
		e.push_back(entry());
		e.back().string() = result[i].second->first.second.to_string();
	}
	return true;
}

bool node_impl::lookup_peers(sha1_hash const& info_hash, int prefix, entry& reply) const
{
	if (m_alerts.should_post<dht_get_peers_alert>())
		m_alerts.post_alert(dht_get_peers_alert(info_hash));

	table_t::const_iterator i = m_map.lower_bound(info_hash);
	if (i == m_map.end()) return false;
	if (i->first != info_hash && prefix == 20) return false;
	if (prefix != 20)
	{
		sha1_hash mask = sha1_hash::max();
		mask <<= (20 - prefix) * 8;
		if ((i->first & mask) != (info_hash & mask)) return false;
	}

	torrent_entry const& v = i->second;
	if (v.peers.empty()) return false;

	if (!v.name.empty()) reply["n"] = v.name;

	int num = (std::min)((int)v.peers.size(), m_settings.max_peers_reply);
	int t = 0;
	int m = 0;
	std::set<peer_entry>::const_iterator iter = v.peers.begin();
	entry::list_type& pe = reply["values"].list();
	std::string endpoint;

	while (m < num)
	{
		if ((random() / float(UINT_MAX + 1.f)) * (num - t) >= num - m)
		{
			++iter;
			++t;
		}
		else
		{
			endpoint.resize(18);
			std::string::iterator out = endpoint.begin();
			write_endpoint(iter->addr, out);
			endpoint.resize(out - endpoint.begin());
			pe.push_back(entry(endpoint));

			++iter;
			++t;
			++m;
		}
	}
	return true;
}

namespace
{
	void write_nodes_entry(entry& r, nodes_t const& nodes)
	{
		bool ipv6_nodes = false;
		entry& n = r["nodes"];
		std::back_insert_iterator<std::string> out(n.string());
		for (nodes_t::const_iterator i = nodes.begin()
			, end(nodes.end()); i != end; ++i)
		{
			if (!i->addr.is_v4())
			{
				ipv6_nodes = true;
				continue;
			}
			std::copy(i->id.begin(), i->id.end(), out);
			write_endpoint(udp::endpoint(i->addr, i->port), out);
		}

		if (ipv6_nodes)
		{
			entry& p = r["nodes2"];
			std::string endpoint;
			for (nodes_t::const_iterator i = nodes.begin()
				, end(nodes.end()); i != end; ++i)
			{
				if (!i->addr.is_v6()) continue;
				endpoint.resize(18 + 20);
				std::string::iterator out = endpoint.begin();
				std::copy(i->id.begin(), i->id.end(), out);
				out += 20;
				write_endpoint(udp::endpoint(i->addr, i->port), out);
				endpoint.resize(out - endpoint.begin());
				p.list().push_back(entry(endpoint));
			}
		}
	}
}

// verifies that a message has all the required
// entries and returns them in ret
bool verify_message(lazy_entry const* msg, key_desc_t const desc[], lazy_entry const* ret[]
	, int size , char* error, int error_size)
{
	// clear the return buffer
	memset(ret, 0, sizeof(ret[0]) * size);

	// when parsing child nodes, this is the stack
	// of lazy_entry pointers to return to
	lazy_entry const* stack[5];
	int stack_ptr = -1;

	if (msg->type() != lazy_entry::dict_t)
	{
		snprintf(error, error_size, "not a dictionary");
		return false;
	}
	++stack_ptr;
	stack[stack_ptr] = msg;
	for (int i = 0; i < size; ++i)
	{
		key_desc_t const& k = desc[i];

//		fprintf(stderr, "looking for %s in %s\n", k.name, print_entry(*msg).c_str());

		ret[i] = msg->dict_find(k.name);
		if (ret[i] && ret[i]->type() != k.type) ret[i] = 0;
		if (ret[i] == 0 && (k.flags & key_desc_t::optional) == 0)
		{
			// the key was not found, and it's not an optiona key
			snprintf(error, error_size, "missing '%s' key", k.name);
			return false;
		}

		if (k.size > 0
			&& ret[i]
			&& k.type == lazy_entry::string_t)
		{
			bool invalid = false;
			if (k.flags & key_desc_t::size_divisible)
				invalid = (ret[i]->string_length() % k.size) != 0;
			else
				invalid = ret[i]->string_length() != k.size;

			if (invalid)
			{
				// the string was not of the required size
				ret[i] = 0;
				if ((k.flags & key_desc_t::optional) == 0)
				{
					snprintf(error, error_size, "invalid value for '%s'", k.name);
					return false;
				}
			}
		}
		if (k.flags & key_desc_t::parse_children)
		{
			TORRENT_ASSERT(k.type == lazy_entry::dict_t);

			if (ret[i])
			{
				++stack_ptr;
				TORRENT_ASSERT(stack_ptr < int(sizeof(stack)/sizeof(stack[0])));
				msg = ret[i];
				stack[stack_ptr] = msg;
			}
			else
			{
				// skip all children
				while (i < size && (desc[i].flags & key_desc_t::last_child) == 0) ++i;
				// if this assert is hit, desc is incorrect
				TORRENT_ASSERT(i < size);
			}
		}
		else if (k.flags & key_desc_t::last_child)
		{
			TORRENT_ASSERT(stack_ptr > 0);
			--stack_ptr;
			msg = stack[stack_ptr];
		}
	}
	return true;
}

void incoming_error(entry& e, char const* msg)
{
	e["y"] = "e";
	entry::list_type& l = e["e"].list();
	l.push_back(entry(203));
	l.push_back(entry(msg));
}

// build response
void node_impl::incoming_request(msg const& m, entry& e)
{
	e = entry(entry::dictionary_t);
	e["y"] = "r";
	e["t"] = m.message.dict_find_string_value("t");

	key_desc_t top_desc[] = {
		{"q", lazy_entry::string_t, 0, 0},
		{"a", lazy_entry::dict_t, 0, 0},
	};

	lazy_entry const* top_level[2];
	char error_string[200];
	if (!verify_message(&m.message, top_desc, top_level, 2, error_string, sizeof(error_string)))
	{
		incoming_error(e, error_string);
		return;
	}

	char const* query = top_level[0]->string_cstr();

	lazy_entry const* arg_ent = top_level[1];

	lazy_entry const* node_id_ent = arg_ent->dict_find_string("id");
	if (node_id_ent == 0 || node_id_ent->string_length() != 20)
	{
		incoming_error(e, "missing 'id' key");
		return;
	}

	node_id id(node_id_ent->string_ptr());

	m_table.heard_about(id, m.addr);

	entry& reply = e["r"];
	m_rpc.add_our_id(reply);

	// if this nodes ID doesn't match its IP, tell it what
	// its IP is
	if (!verify_id(id, m.addr.address()))
		reply["ip"] = address_to_bytes(m.addr.address());

	if (strcmp(query, "ping") == 0)
	{
		// we already have 't' and 'id' in the response
		// no more left to add
	}
	else if (strcmp(query, "get_peers") == 0)
	{
		key_desc_t msg_desc[] = {
			{"info_hash", lazy_entry::string_t, 20, 0},
			{"ifhpfxl", lazy_entry::int_t, 0, key_desc_t::optional},
		};

		lazy_entry const* msg_keys[2];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 2, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

		reply["token"] = generate_token(m.addr, msg_keys[0]->string_ptr());
		
		sha1_hash info_hash(msg_keys[0]->string_ptr());
		nodes_t n;
		// always return nodes as well as peers
		m_table.find_node(info_hash, n, 0);
		write_nodes_entry(reply, n);

		int prefix = msg_keys[1] ? int(msg_keys[1]->int_value()) : 20;
		if (prefix > 20) prefix = 20;
		else if (prefix < 4) prefix = 4;

		bool ret = lookup_peers(info_hash, prefix, reply);
		(void)ret;
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		if (ret) TORRENT_LOG(node) << " values: " << reply["values"].list().size();
#endif
	}
	else if (strcmp(query, "find_node") == 0)
	{
		key_desc_t msg_desc[] = {
			{"target", lazy_entry::string_t, 20, 0},
		};

		lazy_entry const* msg_keys[1];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 1, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

		sha1_hash target(msg_keys[0]->string_ptr());

		// TODO: find_node should write directly to the response entry
		nodes_t n;
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);
	}
	else if (strcmp(query, "announce_peer") == 0)
	{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		extern int g_failed_announces;
#endif
		key_desc_t msg_desc[] = {
			{"info_hash", lazy_entry::string_t, 20, 0},
			{"port", lazy_entry::int_t, 0, 0},
			{"token", lazy_entry::string_t, 0, 0},
			{"n", lazy_entry::string_t, 0, key_desc_t::optional},
		};

		lazy_entry const* msg_keys[4];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 4, error_string, sizeof(error_string)))
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			++g_failed_announces;
#endif
			incoming_error(e, error_string);
			return;
		}

		int port = int(msg_keys[1]->int_value());
		if (port < 0 || port >= 65536)
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			++g_failed_announces;
#endif
			incoming_error(e, "invalid port");
			return;
		}

		sha1_hash info_hash(msg_keys[0]->string_ptr());

		if (m_alerts.should_post<dht_announce_alert>())
			m_alerts.post_alert(dht_announce_alert(
				m.addr.address(), port, info_hash));

		if (!verify_token(msg_keys[2]->string_value(), msg_keys[0]->string_ptr(), m.addr))
		{
#ifdef TORRENT_DHT_VERBOSE_LOGGING
			++g_failed_announces;
#endif
			incoming_error(e, "invalid token");
			return;
		}

		// the token was correct. That means this
		// node is not spoofing its address. So, let
		// the table get a chance to add it.
		m_table.node_seen(id, m.addr);

		if (!m_map.empty() && int(m_map.size()) >= m_settings.max_torrents)
		{
			// we need to remove some. Remove the ones with the
			// fewest peers
			int num_peers = m_map.begin()->second.peers.size();
			table_t::iterator candidate = m_map.begin();
			for (table_t::iterator i = m_map.begin()
				, end(m_map.end()); i != end; ++i)
			{
				if (int(i->second.peers.size()) > num_peers) continue;
				if (i->first == info_hash) continue;
				num_peers = i->second.peers.size();
				candidate = i;
			}
			m_map.erase(candidate);
		}
		torrent_entry& v = m_map[info_hash];

		// the peer announces a torrent name, and we don't have a name
		// for this torrent. Store it.
		if (msg_keys[3] && v.name.empty())
		{
			std::string name = msg_keys[3]->string_value();
			if (name.size() > 50) name.resize(50);
			v.name = name;
		}

		peer_entry peer;
		peer.addr = tcp::endpoint(m.addr.address(), port);
		peer.added = time_now();
		std::set<peer_entry>::iterator i = v.peers.find(peer);
		if (i != v.peers.end()) v.peers.erase(i++);
		v.peers.insert(i, peer);
#ifdef TORRENT_DHT_VERBOSE_LOGGING
		extern int g_announces;
		++g_announces;
#endif
	}
/*
	else if (strcmp(query, "find_torrent") == 0)
	{
		key_desc_t msg_desc[] = {
			{"target", lazy_entry::string_t, 20, 0},
			{"tags", lazy_entry::string_t, 0, 0},
		};

		lazy_entry const* msg_keys[2];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 2, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

		reply["token"] = generate_token(m.addr, msg_keys[0]->string_ptr());

		sha1_hash target(msg_keys[0]->string_ptr());
		nodes_t n;
		// always return nodes as well as torrents
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);

		lookup_torrents(target, reply, (char*)msg_keys[1]->string_cstr());
	}
*/
	else if (strcmp(query, "announce_item") == 0)
	{
		feed_item add_item;
		const static key_desc_t msg_desc[] = {
			{"target", lazy_entry::string_t, 20, 0},
			{"token", lazy_entry::string_t, 0, 0},
			{"sig", lazy_entry::string_t, sizeof(add_item.signature), 0},
			{"head", lazy_entry::dict_t, 0, key_desc_t::optional | key_desc_t::parse_children},
				{"n", lazy_entry::string_t, 0, 0},
				{"key", lazy_entry::string_t, 64, 0},
				{"seq", lazy_entry::int_t, 0, 0},
				{"next", lazy_entry::string_t, 20, key_desc_t::last_child | key_desc_t::size_divisible},
			{"item", lazy_entry::dict_t, 0, key_desc_t::optional | key_desc_t::parse_children},
				{"key", lazy_entry::string_t, 64, 0},
				{"next", lazy_entry::string_t, 20, key_desc_t::last_child | key_desc_t::size_divisible},
		};

		// attempt to parse the message
		lazy_entry const* msg_keys[11];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 11, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

		sha1_hash target(msg_keys[0]->string_ptr());

		// verify the write-token
		if (!verify_token(msg_keys[1]->string_value(), msg_keys[0]->string_ptr(), m.addr))
		{
			incoming_error(e, "invalid token");
			return;
		}

		sha1_hash expected_target;
		sha1_hash item_hash;
		std::pair<char const*, int> buf;
		if (msg_keys[3])
		{
			// we found the "head" entry
			add_item.type = feed_item::list_head;
			add_item.item = *msg_keys[3];

			add_item.name = msg_keys[4]->string_value();
			add_item.sequence_number = msg_keys[6]->int_value();

			buf = msg_keys[3]->data_section();
			item_hash = hasher(buf.first, buf.second).final();

			hasher h;
			h.update(add_item.name);
			h.update((const char*)msg_keys[5]->string_ptr(), msg_keys[5]->string_length());
			expected_target = h.final();
		}
		else if (msg_keys[8])
		{
			// we found the "item" entry
			add_item.type = feed_item::list_item;
			add_item.item = *msg_keys[8];

			buf = msg_keys[8]->data_section();
			item_hash = hasher(buf.first, buf.second).final();
			expected_target = item_hash;
		}
		else
		{
			incoming_error(e, "missing head or item");
			return;
		}

		if (buf.second > 1024)
		{
			incoming_error(e, "message too big");
			return;
		}

		// verify that the key matches the target
		if (expected_target != target)
		{
			incoming_error(e, "invalid target");
			return;
		}

		memcpy(add_item.signature, msg_keys[2]->string_ptr(), sizeof(add_item.signature));

		// #error verify signature by comparing it to item_hash

		m_table.node_seen(id, m.addr);

		feed_table_t::iterator i = m_feeds.find(target);
		if (i == m_feeds.end())
		{
			// make sure we don't add too many items
			if (int(m_feeds.size()) >= m_settings.max_feed_items)
			{
				// delete the least important one (i.e. the one
				// the fewest peers are announcing)
				feed_table_t::iterator j = std::min_element(m_feeds.begin(), m_feeds.end()
					, boost::bind(&feed_item::num_announcers
						, boost::bind(&feed_table_t::value_type::second, _1)));
				TORRENT_ASSERT(j != m_feeds.end());
//				std::cerr << " removing: " << i->second.item << std::endl;
				m_feeds.erase(j);
			}
			boost::tie(i, boost::tuples::ignore) = m_feeds.insert(std::make_pair(target, add_item));
		}
		feed_item& f = i->second;
		if (f.type != add_item.type) return;

		f.last_seen = time_now();
		if (add_item.sequence_number > f.sequence_number)
		{
			f.item.swap(add_item.item);
			f.name.swap(add_item.name);
			f.sequence_number = add_item.sequence_number;
			memcpy(f.signature, add_item.signature, sizeof(f.signature));
		}

		// maybe increase num_announcers if we haven't seen this IP before
		sha1_hash iphash;
		hash_address(m.addr.address(), iphash);
		if (!f.ips.find(iphash))
		{
			f.ips.set(iphash);
			++f.num_announcers;
		}
	}
	else if (strcmp(query, "get_item") == 0)
	{
		key_desc_t msg_desc[] = {
			{"target", lazy_entry::string_t, 20, 0},
			{"key", lazy_entry::string_t, 64, 0},
			{"n", lazy_entry::string_t, 0, key_desc_t::optional},
		};

		// attempt to parse the message
		lazy_entry const* msg_keys[3];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 3, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

		sha1_hash target(msg_keys[0]->string_ptr());

		// verify that the key matches the target
		// we can only do this for list heads, where
		// we have the name.
		if (msg_keys[2])
		{
			hasher h;
			h.update(msg_keys[2]->string_ptr(), msg_keys[2]->string_length());
			h.update(msg_keys[1]->string_ptr(), msg_keys[1]->string_length());
			if (h.final() != target)
			{
				incoming_error(e, "invalid target");
				return;
			}
		}

		reply["token"] = generate_token(m.addr, msg_keys[0]->string_ptr());
		
		nodes_t n;
		// always return nodes as well as peers
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);

		feed_table_t::iterator i = m_feeds.find(target);
		if (i != m_feeds.end())
		{
			feed_item const& f = i->second;

			if (f.type == feed_item::list_head)
				reply["head"] = f.item;
			else
				reply["item"] = f.item;
			reply["sig"] = std::string((char*)f.signature, sizeof(f.signature));
		}
	}
/*
	else if (strcmp(query, "announce_torrent") == 0)
	{
		key_desc_t msg_desc[] = {
			{"target", lazy_entry::string_t, 20, 0},
			{"info_hash", lazy_entry::string_t, 20, 0},
			{"name", lazy_entry::string_t, 0, 0},
			{"tags", lazy_entry::string_t, 0, 0},
			{"token", lazy_entry::string_t, 0, 0},
		};

		lazy_entry const* msg_keys[5];
		if (!verify_message(arg_ent, msg_desc, msg_keys, 5, error_string, sizeof(error_string)))
		{
			incoming_error(e, error_string);
			return;
		}

//		if (m_alerts.should_post<dht_announce_torrent_alert>())
//			m_alerts.post_alert(dht_announce_torrent_alert(
//				m.addr.address(), name, tags, info_hash));

		if (!verify_token(msg_keys[4]->string_value(), msg_keys[0]->string_ptr(), m.addr))
		{
			incoming_error(e, "invalid token in announce");
			return;
		}

		sha1_hash target(msg_keys[0]->string_ptr());
		sha1_hash info_hash(msg_keys[1]->string_ptr());

		// the token was correct. That means this
		// node is not spoofing its address. So, let
		// the table get a chance to add it.
		m_table.node_seen(id, m.addr);

		search_table_t::iterator i = m_search_map.find(std::make_pair(target, info_hash));
		if (i == m_search_map.end())
		{
			boost::tie(i, boost::tuples::ignore)
				= m_search_map.insert(std::make_pair(std::make_pair(target, info_hash)
				, search_torrent_entry()));
		}

		char const* in_tags[20];
		int num_tags = 0;
		num_tags = split_string(in_tags, 20, (char*)msg_keys[3]->string_cstr());

		i->second.publish(msg_keys[2]->string_value(), in_tags, num_tags);
	}
*/
	else
	{
		// if we don't recognize the message but there's a
		// 'target' or 'info_hash' in the arguments, treat it
		// as find_node to be future compatible
		lazy_entry const* target_ent = arg_ent->dict_find_string("target");
		if (target_ent == 0 || target_ent->string_length() != 20)
		{
			target_ent = arg_ent->dict_find_string("info_hash");
			if (target_ent == 0 || target_ent->string_length() != 20)
			{
				incoming_error(e, "unknown message");
				return;
			}
		}

		sha1_hash target(target_ent->string_ptr());
		nodes_t n;
		// always return nodes as well as peers
		m_table.find_node(target, n, 0);
		write_nodes_entry(reply, n);
		return;
	}
}


} } // namespace libtorrent::dht

