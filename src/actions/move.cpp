/*
   Copyright (C) 2003 - 2012 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 * @file
 * Movement.
 */

#include "move.hpp"

#include "vision.hpp"

#include "../game_display.hpp"
#include "../game_events.hpp"
#include "../game_preferences.hpp"
#include "../gettext.hpp"
#include "../hotkeys.hpp"
#include "../log.hpp"
#include "../map.hpp"
#include "../mouse_handler_base.hpp"
#include "../pathfind/pathfind.hpp"
#include "../replay.hpp"
#include "../resources.hpp"
#include "../unit_display.hpp"
#include "../formula_string_utils.hpp"
#include "../team.hpp"
#include "../unit.hpp"
#include "../whiteboard/manager.hpp"

#include <boost/foreach.hpp>
#include <deque>
#include <map>
#include <set>

static lg::log_domain log_engine("engine");
#define DBG_NG LOG_STREAM(debug, log_engine)


void move_unit_spectator::add_seen_friend(const unit_map::const_iterator &u)
{
	seen_friends_.push_back(u);
}


void move_unit_spectator::add_seen_enemy(const unit_map::const_iterator &u)
{
	seen_enemies_.push_back(u);
}


const unit_map::const_iterator& move_unit_spectator::get_ambusher() const
{
	return ambusher_;
}


const unit_map::const_iterator& move_unit_spectator::get_failed_teleport() const
{
	return failed_teleport_;
}


const std::vector<unit_map::const_iterator>& move_unit_spectator::get_seen_enemies() const
{
	return seen_enemies_;
}


const std::vector<unit_map::const_iterator>& move_unit_spectator::get_seen_friends() const
{
	return seen_friends_;
}


const unit_map::const_iterator& move_unit_spectator::get_unit() const
{
	return unit_;
}


move_unit_spectator::move_unit_spectator(const unit_map &units)
	: ambusher_(units.end()),failed_teleport_(units.end()),seen_enemies_(),seen_friends_(),unit_(units.end())
{
}


move_unit_spectator::~move_unit_spectator()
{
}

void move_unit_spectator::reset(const unit_map &units)
{
	ambusher_ = units.end();
	failed_teleport_ = units.end();
	seen_enemies_.clear();
	seen_friends_.clear();
	unit_ = units.end();
}


void move_unit_spectator::set_ambusher(const unit_map::const_iterator &u)
{
	ambusher_ = u;
}


void move_unit_spectator::set_failed_teleport(const unit_map::const_iterator &u)
{
	failed_teleport_ = u;
}


void move_unit_spectator::set_unit(const unit_map::const_iterator &u)
{
	unit_ = u;
}


bool get_village(const map_location& loc, int side, int *action_timebonus)
{
	std::vector<team> &teams = *resources::teams;
	team *t = unsigned(side - 1) < teams.size() ? &teams[side - 1] : NULL;
	if (t && t->owns_village(loc)) {
		return false;
	}

	bool has_leader = resources::units->find_leader(side).valid();
	bool grants_timebonus = false;

	int old_owner_side = 0;
	// We strip the village off all other sides, unless it is held by an ally
	// and we don't have a leader (and thus can't occupy it)
	for(std::vector<team>::iterator i = teams.begin(); i != teams.end(); ++i) {
		int i_side = i - teams.begin() + 1;
		if (!t || has_leader || t->is_enemy(i_side)) {
			if(i->owns_village(loc)) {
				old_owner_side = i_side;
				i->lose_village(loc);
			}
			if (side != i_side && action_timebonus) {
				grants_timebonus = true;
			}
		}
	}

	if (!t) return false;

	if(grants_timebonus) {
		t->set_action_bonus_count(1 + t->action_bonus_count());
		*action_timebonus = 1;
	}

	if(has_leader) {
		if (resources::screen != NULL) {
			resources::screen->invalidate(loc);
		}
		return t->get_village(loc, old_owner_side);
	}

	return false;
}


namespace { // Private helpers for move_unit()

	/// Helper class for move_unit().
	class unit_mover : public boost::noncopyable {
		typedef std::vector<map_location>::const_iterator route_iterator;

	public:
		unit_mover(const std::vector<map_location> & route,
		           move_unit_spectator *move_spectator,
		           bool skip_sightings, const map_location *replay_dest);
		~unit_mover();

		/// Determines how far along the route the unit can expect to move this turn.
		bool check_expected_movement();
		/// Attempts to move the unit along the expected path.
		void try_actual_movement(bool show);
		/// Does some bookkeeping and event firing, for use after movement.
		void post_move(undo_list *undo_stack);
		/// Shows the various on-screen messages, for use after movement.
		void feedback() const;

		/// After checking expected movement, this is the expected path.
		std::vector<map_location> expected_path() const
		{ return std::vector<map_location>(begin_, expected_end_); }
		/// After moving, this is the final hex reached.
		const map_location & final_hex() const
		{ return *move_loc_; }
		/// After moving, this indicates if any units were seen.
		/// This includes seen units that would not interrupt movement.
		bool saw_units() const  { return !seen_units_.empty(); }
		/// The number of hexes actually entered.
		size_t steps_travelled() const
		{ return move_loc_ - begin_; }
		/// After moving, use this to detect if movement was less than expected.
		bool stopped_early() const  { return expected_end_ != real_end_; }
		/// After moving, use this to detect if something happened that would
		/// interrupt movement (even if movement ended for a different reason).
		bool interrupted(bool include_end_of_move_events=true) const
		{
			return ambushed_ || blocked_ || sighted_ || teleport_failed_ ||
			       (include_end_of_move_events ? event_mutated_ : event_mutated_mid_move_ ) ||
			       !move_it_.valid();
		}

	private: // functions
		/// Checks the expected route for hidden units.
		void cache_hidden_units(const route_iterator & start,
		                        const route_iterator & stop);
		/// Fires the enter_hex or exit_hex event and updates our data as needed.
		bool fire_hex_event(const std::string & event_name,
		                    const route_iterator & current,
	                        const route_iterator & other);
		/// AI moves are supposed to not change the "goto" order.
		bool is_ai_move() const	{ return spectator_ != NULL; }
		/// Checks how far it appears we can move this turn.
		route_iterator plot_turn(const route_iterator & start,
		                         const route_iterator & stop);
		/// Updates our stored info after a WML event might have changed something.
		bool post_wml(const route_iterator & step);
		bool post_wml() { return post_wml(full_end_); }
		/// Fires the sighted events that were raised earlier.
		bool pump_sighted(const route_iterator & from);
		/// If ambush_string_ is empty, set it to the "alert" for the given unit.
		void read_ambush_string(const unit_map::const_iterator & ambush_it);

		/// Returns whether or not undoing this move should be blocked.
		bool undo_blocked() const
		{ return ambushed_ || blocked_ || event_mutated_ || fog_changed_ ||
		         saw_units() || teleport_failed_; } // Should not be necessary to check saw_units(), but why assume?

		// The remaining private functions are suggested to be inlined because
		// each is used in only one place. (They are separate functions to ease
		// code reading.)

		/// Checks for ambushers around @a hex, setting flags as appropriate.
		inline void check_for_ambushers(const map_location & hex);
		/// Makes sure the path is not obstructed by a unit.
		inline bool check_for_obstructing_unit(const map_location & hex,
		                                       const map_location & prev_hex);
		/// Moves the unit the next step.
		inline void do_move(const route_iterator & step_from,
		                    const route_iterator & step_to,
		                    unit_display::unit_mover & animator);
		/// Clears fog/shroud and handles units being sighted.
		inline void handle_fog(const map_location & hex, bool ally_interrupts);
		inline bool is_reasonable_stop(const map_location & hex) const;
		/// Reveals the units stored in to_reveal_.
		inline void reveal_ambushers() const;
		/// Makes sure the units in to_reveal_ still exist.
		inline void validate_ambushers();

	private: // data
		// (The order of the fields is somewhat important for the constructor.)

		// Movement parameters (these decrease the number of parameters needed
		// for individual functions).
		move_unit_spectator * const spectator_;
		const bool is_replay_;
		const map_location & replay_dest_;
		const bool skip_sighting_;
		// Needed to interface with unit_display::unit_mover.
		const std::vector<map_location> & route_;

		// The route to traverse.
		const route_iterator begin_;
		const route_iterator full_end_;	// The end of the plotted route.
		route_iterator expected_end_;	// The end of this turn's portion of the plotted route.
		route_iterator ambush_limit_;	// How far we can go before encountering hidden units, ignoring allied units.
		route_iterator obstructed_;	// Points to either full_end_ or a hex we cannot enter. This is used so that exit_hex can fire before we decide we cannot enter this hex.
		route_iterator real_end_;	// How far we actually can move this turn.

		// The unit that is moving.
		unit_map::iterator move_it_;

		// This data stores the state from before the move started.
		const int orig_side_;
		const int orig_moves_;
		const map_location::DIRECTION orig_dir_;
		const map_location goto_;

		// This data tracks the current state as the move is in proress.
		int current_side_;
		team * current_team_;	// Will default to the original team if the moving unit becomes invalid.
		bool current_uses_fog_;
		route_iterator move_loc_; // Will point to the last moved-to location (in case the moving unit disappears).
		size_t do_move_track_;	// Tracks whether or not do_move() needs to update the displayed (fake) unit. Should only be touched by do_move() and the constructor.

		// Data accumulated while making the move.
		map_location zoc_stop_;
		map_location ambush_stop_; // Could be inaccurate if ambushed_ is false.
		bool ambushed_;
		bool blocked_; // Blocked by an enemy (non-ambusher) unit
		bool event_mutated_;
		bool event_mutated_mid_move_; // Cache of event_mutated_ from just before the end-of-move handling.
		bool fog_changed_;
		bool sighted_;	// Records if sightings were made that could interrupt movement.
		bool sighted_stop_;	// Records if sightings were made that did interrupt movement (the same as sighted_ unless movement ended for another reason).
		bool teleport_failed_;
		bool report_extra_hex_;
		std::string ambush_string_;
		std::map<map_location, int> jamming_;
		std::deque<int> moves_left_;	// The front value is what the moving unit's remaining moves should be set to after the next step through the route.
		std::set<map_location> seen_units_;
		std::vector<map_location> to_reveal_;
	};


	/// This constructor assumes @a route is not empty, and it will assert() that
	/// there is a unit at route.front().
	/// Iterators into @a route must remain valid for the life of this object.
	/// It is assumed that move_spectator is only supplied for AI moves (only
	/// affects whether or not gotos are changed).
	unit_mover::unit_mover(const std::vector<map_location> & route,
	                       move_unit_spectator *move_spectator,
	                       bool skip_sightings, const map_location *replay_dest) :
		spectator_(move_spectator),
		is_replay_(replay_dest != NULL),
		replay_dest_(is_replay_ ? *replay_dest : route.back()),
		skip_sighting_(is_replay_ || skip_sightings),
		route_(route),
		begin_(route.begin()),
		full_end_(route.end()),
		expected_end_(begin_),
		ambush_limit_(begin_),
		obstructed_(full_end_),
		real_end_(begin_),
		// Unit information:
		move_it_(resources::units->find(*begin_)),
		orig_side_(( assert(move_it_ != resources::units->end()),
		             move_it_->side() )),
		orig_moves_(move_it_->movement_left()),
		orig_dir_(move_it_->facing()),
		goto_( is_ai_move() ? move_it_->get_goto() : route.back() ),
		current_side_(orig_side_),
		current_team_(&(*resources::teams)[current_side_-1]),
		current_uses_fog_(current_team_->fog_or_shroud()  &&
		                  current_team_->auto_shroud_updates()),
		move_loc_(begin_),
		do_move_track_(game_events::wml_tracking()),
		// The remaining fields are set to some sort of "zero state".
		zoc_stop_(map_location::null_location),
		ambush_stop_(map_location::null_location),
		ambushed_(false),
		blocked_(false),
		event_mutated_(false),
		event_mutated_mid_move_(false),
		fog_changed_(false),
		sighted_(false),
		sighted_stop_(false),
		teleport_failed_(false),
		report_extra_hex_(false),
		ambush_string_(),
		jamming_(),
		moves_left_(),
		seen_units_(),
		to_reveal_()
	{
		if ( !is_ai_move() )
			// Clear the "goto" instruction during movement.
			// (It will be reset in the destructor if needed.)
			move_it_->set_goto(map_location::null_location);
	}


	unit_mover::~unit_mover()
	{
		// Set the "goto" order? (Not if WML set it.)
		if ( !is_ai_move()  &&  move_it_.valid()  &&
		     move_it_->get_goto() == map_location::null_location )
		{
			// Only set the goto if movement was not complete and was not
			// interrupted.
			if ( real_end_ != full_end_  &&  !interrupted(false) ) // End-of-move-events do not cancel a goto. (Use case: tutorial S2.)
				move_it_->set_goto(goto_);
		}
	}


	// Private inlines:

	/**
	 * Checks for ambushers around @a hex, setting flags as appropriate.
	 */
	inline void unit_mover::check_for_ambushers(const map_location & hex)
	{
		const unit_map &units = *resources::units;

		// Need to check each adjacent hex for hidden enemies.
		map_location adjacent[6];
		get_adjacent_tiles(hex, adjacent);
		for ( int i = 0; i != 6; ++i )
		{
			const unit_map::const_iterator neighbor_it = units.find(adjacent[i]);

			if ( neighbor_it != units.end()  &&
			     current_team_->is_enemy(neighbor_it->side())  &&
			     neighbor_it->invisible(adjacent[i]) )
			{
				// Ambushed!
				ambushed_ = true;
				ambush_stop_ = hex;
				to_reveal_.push_back(adjacent[i]);

				// Get a feedback message (first one available).
				if ( ambush_string_.empty() )
					read_ambush_string(neighbor_it);
			}
		}
	}


	/**
	 * Makes sure the path is not obstructed by a unit.
	 * @param  hex       The hex to check.
	 * @param  prev_hex  The previous hex in the route (used to detect a teleport).
	 * @return true if @a hex is obstructed.
	 */
	inline bool unit_mover::check_for_obstructing_unit(const map_location & hex,
	                                                   const map_location & prev_hex)
	{
		const unit_map::const_iterator blocking_unit = resources::units->find(hex);

		// If no unit, then the path is not obstructed.
		if ( blocking_unit == resources::units->end() )
			return false;

		if ( !tiles_adjacent(hex, prev_hex) ) {
			// Cannot teleport to an occupied hex.
			teleport_failed_ = true;
			return true;
		}

		if ( current_team_->is_enemy(blocking_unit->side()) ) {
			// Trying to go through an enemy.
			blocked_ = true;
			to_reveal_.push_back(hex);
			return true;
		}

		// If we get here, the unit does not interfere with movement.
		return false;
	}


	/**
	 * Moves the unit the next step.
	 * @a step_to is the hex being moved to.
	 * @a step_from is the hex before that in the route.
	 * (The unit is actually at *move_loc_.)
	 * @a animator is the unit_display::unit_mover being used.
	 */
	inline void unit_mover::do_move(const route_iterator & step_from,
	                                const route_iterator & step_to,
	                                unit_display::unit_mover & animator)
	{
		game_display &disp = *resources::screen;

		// Adjust the movement even if we cannot move yet.
		// We will eventually be able to move if nothing unexpected
		// happens, and if something does happen, this movement is the
		// cost to discover it.
		move_it_->set_movement(moves_left_.front());
		moves_left_.pop_front();

		// Attempt actually moving.
		// (Fails if *step_to is occupied).
		std::pair<unit_map::iterator, bool> move_result =
			resources::units->move(*move_loc_, *step_to);
		if ( move_result.second )
		{
			// Update the moving unit.
			move_it_ = move_result.first;
			move_it_->set_facing(step_from->get_relative_dir(*step_to));
			move_it_->set_standing(false);
			disp.invalidate_unit_after_move(*move_loc_, *step_to);
			disp.invalidate(*move_loc_);
			disp.invalidate(*step_to);
			move_loc_ = step_to;

			// Show this move.
			const size_t current_tracking = game_events::wml_tracking();
			animator.proceed_to(*move_it_, step_to - begin_,
			                    current_tracking != do_move_track_);
			do_move_track_ = current_tracking;
			disp.redraw_minimap();
		}
	}


	/**
	 * Clears fog/shroud and raises events for units being sighted.
	 * Only call this if the current team uses fog or shroud.
	 * @a hex is both the center of fog clearing and the assumed location of
	 * the moving unit when the sighted events will be fired.
	 */
	inline void unit_mover::handle_fog(const map_location & hex, bool ally_interrupts)
	{
		static const std::string sighted_str("sighted");

		const unit_map & units = *resources::units;

		std::set<map_location> newly_seen_units;
		std::set<map_location> petrified_units;

		// Clear the fog.
		if ( clear_shroud_unit(hex, *move_it_, *current_team_, jamming_, NULL,
		                       &newly_seen_units, &petrified_units) )
		{
			invalidate_after_clearing_shroud();
			fog_changed_ = true;
		}

		// Raise sighted events.
		const game_events::entity_location viewer(*move_it_, hex);
		BOOST_FOREACH(const map_location &here, newly_seen_units) {
			game_events::raise(sighted_str, here, viewer);
		}
		BOOST_FOREACH(const map_location &here, petrified_units) {
			game_events::raise(sighted_str, here, viewer);
		}

		// Check for sighted units?
		if ( !skip_sighting_ && !sighted_ ) {
			if ( ally_interrupts )
				// Interrupt if any unit was sighted.
				sighted_ = !newly_seen_units.empty();
			else {
				// Check whether any sighted unit is an enemy.
				BOOST_FOREACH(const map_location &here, newly_seen_units) {
					if ( current_team_->is_enemy(units.find(here)->side()) ) {
						sighted_ = true;
						break;
					}
				}
			}
		}

		// Merge the seen units.
		seen_units_.insert(newly_seen_units.begin(), newly_seen_units.end());
	}


	/**
	 * @return true if an unscheduled stop at @a hex is not likely to negatively
	 * impact the player's plans.
	 * (E.g. it would not kill movement by making an unintended village capture.)
	 */
	inline bool unit_mover::is_reasonable_stop(const map_location & hex) const
	{
		// We cannot reasonably stop if move_it_ could not be moved to this
		// hex (the hex was occupied by someone else).
		if ( *move_loc_ != hex )
			return false;

		// We can reasonably stop if the hex is not an unowned village.
		return !resources::game_map->is_village(hex) ||
		       current_team_->owns_village(hex);
	}


	/**
	 * Reveals the units stored in to_reveal_.
	 * Only call this if appropriate; this function does not itself check
	 * ambushed_ or blocked_.
	 */
	inline void unit_mover::reveal_ambushers() const
	{
		// Some convenient aliases:
		game_display &disp = *resources::screen;
		unit_map &units = *resources::units;

		BOOST_FOREACH(const map_location & reveal, to_reveal_) {
			unit_map::iterator ambusher = units.find(reveal);
			if ( ambusher != units.end() ) {
				ambusher->set_state(unit::STATE_UNCOVERED, true);  // (Needed in case we backtracked.)
				if ( spectator_ )
					spectator_->set_ambusher(ambusher);
			}
			disp.invalidate(reveal);
		}

		disp.draw();
	}


	/**
	 * Makes sure the units in to_reveal_ still exist.
	 * Also updates ambush_string_ based on those that do still exist.
	 */
	inline void unit_mover::validate_ambushers()
	{
		const unit_map &units = *resources::units;

		ambush_string_.clear();

		// Loop through the previously-detected ambushers.
		size_t i = 0;
		while ( i != to_reveal_.size() ) {
			const unit_map::const_iterator ambush_it = units.find(to_reveal_[i]);
			if ( ambush_it == units.end() )
				// Ambusher is gone.
				to_reveal_.erase(to_reveal_.begin() + i);
			else {
				// Update ambush_string_ and proceed to the next ambusher.
				read_ambush_string(ambush_it);
				++i;
			}
		}
	}


	// Private utilities:

	/**
	 * Checks the expected route for hidden units.
	 * This basically handles all the checks for surprises that can be done
	 * without visibly notifying a player. Thus this can be called at the
	 * start of movement and immediately after events, rather than tie up
	 * CPU time in the middle of animating a move.
	 *
	 * @param[in]  start           The beginning of the path to check.
	 * @param[in]  stop            The end of the path to check.
	 */
	void unit_mover::cache_hidden_units(const route_iterator & start,
	                                    const route_iterator & stop)
	{
		// Clear the old cache.
		obstructed_ = full_end_;
		blocked_ = false;
		teleport_failed_ = false;
		jamming_.clear();
		// The ambush cache needs special treatment since we cannot re-detect
		// an ambush if we are already at the ambushed location.
		ambushed_ =  ambushed_  &&  ambush_stop_ == *start;
		if ( ambushed_ ) {
			validate_ambushers();
			ambushed_ = !to_reveal_.empty();
		}
		if ( !ambushed_ ) {
			ambush_stop_ = map_location::null_location;
			ambush_string_.clear();
			to_reveal_.clear();
		}

		// Calculate the jamming map?
		if ( current_uses_fog_ )
			calculate_jamming(current_side_, jamming_);


		// Abort for null routes.
		if ( start == stop ) {
			ambush_limit_ = start;
			return;
		}

		// This loop will end with ambush_limit_ pointing one element beyond
		// where the unit would be forced to stop by a hidden unit.
		for ( ambush_limit_ = start+1; ambush_limit_ != stop; ++ambush_limit_ ) {
			// Check if we need to stop in the previous hex.
			if ( ambushed_ )
				if ( !is_replay_  ||  *(ambush_limit_-1) == replay_dest_ )
					break;

			// Check for being unable to enter this hex.
			if ( check_for_obstructing_unit(*ambush_limit_, *(ambush_limit_-1)) ) {
				// No replay check here? Makes some sense, I guess.
				obstructed_ = ambush_limit_++; // The limit needs to be after obstructed_ in order for the latter to do anything.
				break;
			}

			// We can enter this hex.
			// See if we are stopped in this hex.
			check_for_ambushers(*ambush_limit_);
		}
	}


	/**
	 * Fires the enter_hex or exit_hex event and updates our data as needed.
	 *
	 * @param[in]  event_name  The name of the event ("enter_hex" or "exit_hex").
	 * @param[in]  current     The currently occupied hex.
	 * @param[in]  other       The secondary hex to provide to the event.
	 *
	 * @return true if this event should interrupt movement.
	 * (This is also stored in event_mutated_.)
	 */
	bool unit_mover::fire_hex_event(const std::string & event_name,
		                            const route_iterator & current,
	                                const route_iterator & other)
	{
		const size_t track = game_events::wml_tracking();
		bool valid = true;

		const game_events::entity_location mover(*move_it_, *current);
		const bool event = game_events::fire(event_name, mover, *other);

		if ( track != game_events::wml_tracking() )
			// Some WML fired, so update our status.
			valid = post_wml(current);

		if ( event || !valid )
			event_mutated_ = true;

		return event || !valid;
	}


	/**
	 * Checks how far it appears we can move this turn.
	 *
	 * @param[in] start         The beginning of the plotted path.
	 * @param[in] stop          The end of the plotted path.
	 *
	 * @return An end iterator for the path that can be traversed this turn.
	 */
	unit_mover::route_iterator unit_mover::plot_turn(const route_iterator & start,
	                                                 const route_iterator & stop)
	{
		const gamemap &map = *resources::game_map;

		// Handle null routes.
		if ( start == stop )
			return start;


		int remaining_moves = move_it_->movement_left();
		zoc_stop_ = map_location::null_location;
		moves_left_.clear();

		if ( start != begin_ ) {
			// Check for being unable to leave the current hex.
			if ( !move_it_->get_ability_bool("skirmisher", *start)  &&
			      pathfind::enemy_zoc(*current_team_, *start, *current_team_) )
				zoc_stop_ = *start;
		}

		// This loop will end with end pointing one element beyond where the
		// unit thinks it will stop (the usual notion of "end" for iterators).
		route_iterator end = start + 1;
		for ( ; end != stop; ++end )
		{
			// Break out of the loop if we cannot leave the previous hex.
			if ( zoc_stop_ != map_location::null_location  &&  !is_replay_ )
				break;
			remaining_moves -= move_it_->movement_cost(map[*end]);
			if ( remaining_moves < 0 ) {
				if ( is_replay_ )
					remaining_moves = 0;
				else
					break;
			}

			// We can enter this hex. Record the cost.
			moves_left_.push_back(remaining_moves);

			// Check for being unable to leave this hex.
			if ( !move_it_->get_ability_bool("skirmisher", *end)  &&
			      pathfind::enemy_zoc(*current_team_, *end, *current_team_) )
				zoc_stop_ = *end;
		}

		if ( !is_replay_ ) {
			// Avoiding stopping on a (known) unit.
			route_iterator min_end =  start == begin_ ? start : start + 1;
			while ( end != min_end  &&  get_visible_unit(*(end-1), *current_team_) )
				// Backtrack.
				--end;
		}

		return end;
	}


	/**
	 * Updates our stored info after a WML event might have changed something.
	 *
	 * @param step      Indicates the position in the path where we might need to start recalculating movement.
	 *                  Set this to full_end_ (or do not supply it) to skip such recalculations (because movement has finished).
	 *
	 * @returns false if continuing is impossible (i.e. we lost the moving unit).
	 */
	bool unit_mover::post_wml(const route_iterator & step)
	{
		// Re-find the moving unit.
		move_it_ = resources::units->find(*move_loc_);
		const bool found = move_it_ != resources::units->end();

		// Update the current unit data.
		current_side_ = found ? move_it_->side() : orig_side_;
		current_team_ = &(*resources::teams)[current_side_-1];
		current_uses_fog_ = current_team_->fog_or_shroud()  &&
		                    ( current_side_ != orig_side_  ||
		                      current_team_->auto_shroud_updates() );

		// Update the path.
		if ( found  &&  step != full_end_ ) {
			const route_iterator new_limit = plot_turn(step, expected_end_);
			cache_hidden_units(step, new_limit);
			// Just in case: length 0 paths become length 1 paths.
			if ( ambush_limit_ == step )
				++ambush_limit_;
		}

		return found;
	}


	/**
	 * Fires the sighted events that were raised earlier.
	 *
	 * @param[in]  from  Points to the hex the sighting unit currently occupies.
	 *
	 * @return true if this event should interrupt movement.
	 */
	bool unit_mover::pump_sighted(const route_iterator & from)
	{
		const size_t track = game_events::wml_tracking();
		bool valid = true;

		const bool event = game_events::pump();

		if ( track != game_events::wml_tracking() )
			// Some WML fired, so update our status.
			valid = post_wml(from);

		if ( event || !valid )
			event_mutated_ = true;

		return event || !valid;
	}


	/**
	 * If ambush_string_ is empty, set it to the "alert" for the given unit.
	 */
	void unit_mover::read_ambush_string(const unit_map::const_iterator & ambush_it)
	{
		const unit_ability_list hides = ambush_it->get_abilities("hides");
		std::vector<std::pair<const config *, map_location> >::const_iterator hide_it;
		for ( hide_it = hides.cfgs.begin();
		      hide_it != hides.cfgs.end()  &&  ambush_string_.empty();
		      ++hide_it )
		{
			ambush_string_ = (*hide_it->first)["alert"].str();
		}
	}


	// Public interface:

	/**
	 * Determines how far along the route the unit can expect to move this turn.
	 * This is based solely on data known to the player, and will not plot a move
	 * that ends on another (known) unit.
	 * (For example, this prevents a player from plotting a multi-turn move that
	 * has this turn's movement ending on a (slower) unit, and thereby clearing
	 * fog as if the moving unit actually made it on top of that other unit.)
	 *
	 * @returns false if the expectation is to not move at all.
	 */
	bool unit_mover::check_expected_movement()
	{
		expected_end_ = plot_turn(begin_, full_end_);
		return expected_end_ != begin_;
	}


	/**
	 * Attempts to move the unit along the expected path.
	 * (This will do nothing unless check_expected_movement() was called first.)
	 *
	 * @param[in]  show            Set to false to suppress animations.
	 */
	void unit_mover::try_actual_movement(bool show)
	{
		static const std::string enter_hex_str("enter_hex");
		static const std::string exit_hex_str("exit_hex");

		const bool ally_interrupts = preferences::interrupt_when_ally_sighted();

		bool obstructed_stop = false;


		// Check for hidden units along the expected path before we start
		// animating and firing events.
		cache_hidden_units(begin_, expected_end_);

		if ( begin_ != ambush_limit_ ) {
			// Prepare to animate.
			unit_display::unit_mover animator(route_, show);
			animator.start(*move_it_);

			// Traverse the route to the hex where we need to stop.
			// Each iteration performs the move from real_end_-1 to real_end_.
			for ( real_end_ = begin_+1; real_end_ != ambush_limit_; ++real_end_ ) {
				const route_iterator step_from = real_end_ - 1;
				const bool can_break = !is_replay_  ||  replay_dest_ == *step_from;

				// See if we can leave *step_from.
				// Already accounted for: ambusher
				if ( event_mutated_  &&  can_break )
					break;
				if ( sighted_ && can_break && is_reasonable_stop(*step_from) )
				{
					sighted_stop_ = true;
					break;
				}
				// Already accounted for: ZoC
				// Already accounted for: movement cost
				if ( fire_hex_event(exit_hex_str, step_from, real_end_) ) {
					if ( can_break ) {
						report_extra_hex_ = true;
						break;
					}
				}
				if ( real_end_ == obstructed_ ) {
					// We did not check for being a replay when checking for an
					// obstructed hex, so we do not check can_break here.
					report_extra_hex_ = true;
					obstructed_stop = true;
					break;
				}
				if ( is_replay_  &&  replay_dest_ == *step_from )
					// Preserve the replay.
					break;

				// We can leave *step_from. Make the move to *real_end_.
				do_move(step_from, real_end_, animator);

				// Update the fog.
				if ( current_uses_fog_ )
					handle_fog(*real_end_, ally_interrupts);

				// Fire the events for this move.
				pump_sighted(real_end_); // Ignore the return value, as there are more events to try to fire, and real_end_ still needs to be incremented.
				fire_hex_event(enter_hex_str, real_end_, step_from); // Ignore the return value, as real_end_ still needs to be incremented.
			}//for
			if ( move_it_.valid() )
				animator.finish(*move_it_);
		}//if

		// Some flags were set to indicate why we might stop.
		// Update those to reflect whether or not we got to them.
		ambushed_ = ambushed_ && real_end_ == ambush_limit_;
		blocked_  = blocked_  && obstructed_stop;
		teleport_failed_ = teleport_failed_ && obstructed_stop;
		// event_mutated_ does not get unset, regardless of other reasons
		// for stopping, but we do save its current value.
		event_mutated_mid_move_ = event_mutated_;

		// Need the default ambush message?
		if ( ambushed_  &&  ambush_string_.empty() )
			ambush_string_ = _("Ambushed!");
	}


	/**
	 * Does some bookkeeping and event firing, for use after movement.
	 * This includes village capturing and the undo stack.
	 */
	void unit_mover::post_move(undo_list *undo_stack)
	{
		const map_location & final_loc = final_hex();

		int orig_village_owner = -1;
		int action_time_bonus = 0;

		// Reveal ambushers?
		if ( ambushed_ || blocked_ )
			reveal_ambushers();
		else if ( teleport_failed_ && spectator_ )
			spectator_->set_failed_teleport(resources::units->find(*obstructed_));
		unit::clear_status_caches();

		if ( move_it_.valid() ) {
			// Update the moving unit.
			move_it_->set_interrupted_move(
				sighted_stop_ && !resources::whiteboard->is_executing_actions() ?
					*(full_end_-1) :
					map_location::null_location);
			if ( ambushed_ || final_loc == zoc_stop_ )
				move_it_->set_movement(0);

			// Village capturing.
			if ( resources::game_map->is_village(final_loc) ) {
				// Is this a capture?
				orig_village_owner = village_owner(final_loc);
				if ( orig_village_owner != current_side_-1 ) {
					// Captured. Zap movement and take over the village.
					move_it_->set_movement(0);
					event_mutated_ |= get_village(final_loc, current_side_, &action_time_bonus);
					post_wml();
				}
			}
		}

		// Finally, the moveto event.
		event_mutated_ |= game_events::fire("moveto", final_loc, *begin_);
		post_wml();

		// Record keeping.
		if ( spectator_ )
			spectator_->set_unit(move_it_);
		if ( undo_stack ) {
			const bool mover_valid = move_it_.valid();

			if ( mover_valid ) {
				// MP_COUNTDOWN: added param
				undo_stack->push_back(
					undo_action(*move_it_, begin_, real_end_, orig_moves_,
						action_time_bonus, orig_village_owner, orig_dir_));
			}

			if ( !mover_valid  ||  undo_blocked()  ||
			    (resources::whiteboard->is_active() && resources::whiteboard->should_clear_undo()) )
			{
				apply_shroud_changes(*undo_stack, orig_side_);
				undo_stack->clear();
			}
		}

		// Update the screen.
		resources::screen->redraw_minimap();
		resources::screen->draw();
	}


	/**
	 * Shows the various on-screen messages, for use after movement.
	 */
	void unit_mover::feedback() const
	{
		// Alias some resources.
		const unit_map &units = *resources::units;
		game_display &disp = *resources::screen;

		bool redraw = false;
		bool playing_team_is_viewing = disp.playing_team() == disp.viewing_team()
		                               ||  disp.show_everything();

		// Multiple messages may be displayed simultaneously
		// this variable is used to keep them from overlapping
		std::string message_prefix = "";

		// Ambush feedback?
		if ( ambushed_ ) {
			// Suppress the message for observers if the ambusher(s) cannot be seen.
			bool show_message = playing_team_is_viewing;
			BOOST_FOREACH(const map_location &ambush, to_reveal_) {
				if ( !disp.fogged(ambush) )
					show_message = true;
			}
			if ( show_message ) {
				disp.announce(message_prefix + ambush_string_, font::BAD_COLOR);
				message_prefix += " \n";
				redraw = true;
			}
		}

		// Failed teleport feedback?
		if ( playing_team_is_viewing  &&  teleport_failed_ ) {
			std::string teleport_string = _("Failed teleport! Exit not empty");
			disp.announce(message_prefix + teleport_string, font::BAD_COLOR);
			message_prefix += " \n";
			redraw = true;
		}

		// Sighted units feedback?
		if ( playing_team_is_viewing  &&  saw_units() ) {
			// Count the number of allies and enemies sighted.
			int friends = 0, enemies = 0;
			BOOST_FOREACH(const map_location &loc, seen_units_) {
				DBG_NG << "Processing unit at " << loc << "...\n";
				const unit_map::const_iterator seen_it = units.find(loc);

				// Unit may have been removed by an event.
				if ( seen_it == units.end() ) {
					DBG_NG << "...was removed.\n";
					continue;
				}

				if ( current_team_->is_enemy(seen_it->side()) ) {
					++enemies;
					if ( spectator_ )
						spectator_->add_seen_enemy(seen_it);
				} else {
					++friends;
					if ( spectator_ )
						spectator_->add_seen_friend(seen_it);
				}
				DBG_NG << "...processed.\n";
			}

			// Create the message to display (depends on whether firends,
			// enemies, or both were sighted, and on how many of each).
			utils::string_map symbols;
			symbols["friends"] = lexical_cast<std::string>(friends);
			symbols["enemies"] = lexical_cast<std::string>(enemies);
			std::string message;
			SDL_Color msg_color;
			if ( friends > 0  &&  enemies > 0 ) {
				// Both friends and enemies sighted -- neutral message.
				symbols["friendphrase"] = vngettext("Part of 'Units sighted! (...)' sentence^1 friendly", "$friends friendly", friends, symbols);
				symbols["enemyphrase"] = vngettext("Part of 'Units sighted! (...)' sentence^1 enemy", "$enemies enemy", enemies, symbols);
				message = vgettext("Units sighted! ($friendphrase, $enemyphrase)", symbols);
				msg_color = font::NORMAL_COLOR;
			} else if ( friends > 0 ) {
				// Only friends sighted -- good message.
				message = vngettext("Friendly unit sighted", "$friends friendly units sighted", friends, symbols);
				msg_color = font::GOOD_COLOR;
			} else if ( enemies > 0 ) {
				// Only enemies sighted -- bad message.
				message = vngettext("Enemy unit sighted!", "$enemies enemy units sighted!", enemies, symbols);
				msg_color = font::BAD_COLOR;
			}

			disp.announce(message_prefix + message, msg_color);
			message_prefix += " \n";
			redraw = true;
		}

		// Suggest "continue move"?
		if ( playing_team_is_viewing && sighted_stop_ && !resources::whiteboard->is_executing_actions() ) {
			// See if the "Continue Move" action has an associated hotkey
			const hotkey::hotkey_item& hk = hotkey::get_hotkey(hotkey::HOTKEY_CONTINUE_MOVE);
			if ( !hk.null() ) {
				utils::string_map symbols;
				symbols["hotkey"] = hk.get_name();
				std::string message = vgettext("(press $hotkey to keep moving)", symbols);
				disp.announce(message_prefix + message, font::NORMAL_COLOR);
				message_prefix += " \n";
				redraw = true;
			}
		}

		// Update the screen.
		if ( redraw )
			disp.draw();
	}

}//end anonymous namespace

/**
 * Moves a unit across the board.
 *
 * This function handles actual movement, checking terrain costs as well as
 * things that might interrupt movement (e.g. ambushes). If the full path
 * cannot be reached this turn, the remainder is stored as the unit's "goto"
 * instruction. (The unit itself is whatever unit is at the beginning of the
 * supplied path.)
 *
 * @param[in]  steps                The route to be travelled. The unit to be moved is at the beginning of this route.
 * @param[out] move_recorder        Will be given the route actually travelled (which might be shorter than the route specified) so it can be stored in the replay.
 * @param      undo_stack           If supplied, then either this movement will be added to the stack or the stack will be cleared.
 * @param[in]  continued_move       If set to true, this is a continuation of an earlier move (movement is not interrupted should units be spotted).
 * @param[in]  show_move            Controls whether or not the movement is animated for the player.
 * @param[out] interrupted          If supplied, then this is set to true if information was uncovered that warrants interrupting a chain of actions (and set to false otherwise).
 * @param[out] move_spectator       If supplied, this will be given the information uncovered by the move (and the unit's "goto" instruction will be preserved).
 * @param[in]  replay_dest          If not NULL, then this move is assumed to be a replay that expects the unit to be moved to here. Several normal considerations are ignored in a replay.
 *
 * @returns The number of hexes entered. This can safely be used as an index
 *          into @a steps to get the location where movement ended, provided
 *          @a steps is not empty (the return value is guaranteed to be less
 *          than steps.size() ).
 */
size_t move_unit(const std::vector<map_location> &steps,
                 replay* move_recorder, undo_list* undo_stack,
                 bool continued_move, bool show_move,
                 bool* interrupted,
                 move_unit_spectator* move_spectator,
                 const map_location* replay_dest)
{
	const events::command_disabler disable_commands;

	// Default return value.
	if ( interrupted )
		*interrupted = false;

	// Avoid some silliness.
	if ( steps.size() < 2  ||  (steps.size() == 2 && steps.front() == steps.back()) ) {
		DBG_NG << "Ignoring a unit trying to jump on its hex at " <<
		          ( steps.empty() ? map_location::null_location : steps.front() ) << ".\n";
		return 0;
	}

	// Evaluate this move.
	unit_mover mover(steps, move_spectator, continued_move, replay_dest);
	if ( !mover.check_expected_movement() )
		return 0;
	if ( move_recorder )
		// Record the expected movement, so that replays trigger the same events.
		// (Recorded here in case an exception occurs during movement.)
		move_recorder->add_movement(mover.expected_path());

	// Attempt moving.
	mover.try_actual_movement(show_move);
	if ( move_recorder  &&  mover.stopped_early() )
		// Record the early stop.
		move_recorder->limit_movement(mover.final_hex());

	// Bookkeeping, etc.
	mover.post_move(undo_stack);
	if ( show_move )
		mover.feedback();

	// Set return value.
	if ( interrupted )
		*interrupted = mover.interrupted();

	return mover.steps_travelled();
}


bool unit_can_move(const unit &u)
{
	const team &current_team = (*resources::teams)[u.side() - 1];

	if(!u.attacks_left() && u.movement_left()==0)
		return false;

	// Units with goto commands that have already done their gotos this turn
	// (i.e. don't have full movement left) should have red globes.
	if(u.has_moved() && u.has_goto()) {
		return false;
	}

	map_location locs[6];
	get_adjacent_tiles(u.get_location(), locs);
	for(int n = 0; n != 6; ++n) {
		if (resources::game_map->on_board(locs[n])) {
			const unit_map::const_iterator i = resources::units->find(locs[n]);
			if (i.valid() && !i->incapacitated() &&
			    current_team.is_enemy(i->side())) {
				return true;
			}

			if (u.movement_cost((*resources::game_map)[locs[n]]) <= u.movement_left()) {
				return true;
			}
		}
	}

	return false;
}

