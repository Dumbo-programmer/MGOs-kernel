#include <thor-internal/cpu-data.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

static constexpr bool logTimers = false;
static constexpr bool logProgress = false;

ClockSource *globalClockSource;
PrecisionTimerEngine *globalTimerEngine;

PrecisionTimerEngine::PrecisionTimerEngine(ClockSource *clock, AlarmTracker *alarm)
: _clock{clock}, _alarm{alarm} {
	_alarm->setSink(this);
}

void PrecisionTimerEngine::installTimer(PrecisionTimerNode *timer) {
	assert(!timer->_engine);
	timer->_engine = this;

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);
	assert(timer->_state == TimerState::none);

	if(logTimers) {
		auto current = _clock->currentNanos();
		infoLogger() << "thor: Setting timer at " << timer->_deadline
				<< " (counter is " << current << ")" << frg::endlog;
	}

//	infoLogger() << "thor: Active timers: " << _activeTimers << frg::endlog;

	if(!timer->_cancelCb.try_set(timer->_cancelToken)) {
		timer->_wasCancelled = true;
		timer->_state = TimerState::retired;
		WorkQueue::post(timer->_elapsed);
		return;
	}

	_timerQueue.push(timer);
	_activeTimers++;
	timer->_state = TimerState::queued;

	_progress();
}

void PrecisionTimerEngine::cancelTimer(PrecisionTimerNode *timer) {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	if(timer->_state == TimerState::queued) {
		_timerQueue.remove(timer);
		_activeTimers--;
		timer->_wasCancelled = true;
	}else{
		assert(timer->_state == TimerState::elapsed);
	}

	timer->_state = TimerState::retired;
	WorkQueue::post(timer->_elapsed);
}

void PrecisionTimerEngine::firedAlarm() {
	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	_progress();
}

// This function is somewhat complicated because we have to avoid a race between
// the comparator setup and the main counter.
void PrecisionTimerEngine::_progress() {
	auto current = _clock->currentNanos();
	do {
		// Process all timers that elapsed in the past.
		if(logProgress)
			infoLogger() << "thor: Processing timers until " << current << frg::endlog;
		while(true) {
			if(_timerQueue.empty()) {
				_alarm->arm(0);
				return;
			}

			if(_timerQueue.top()->_deadline > current)
				break;

			auto timer = _timerQueue.top();
			assert(timer->_state == TimerState::queued);
			_timerQueue.pop();
			_activeTimers--;
			if(logProgress)
				infoLogger() << "thor: Timer completed" << frg::endlog;
			if(timer->_cancelCb.try_reset()) {
				timer->_state = TimerState::retired;
				WorkQueue::post(timer->_elapsed);
			}else{
				// Let the cancellation handler invoke the continuation.
				timer->_state = TimerState::elapsed;
			}
		}

		// Setup the comparator and iterate if there was a race.
		assert(!_timerQueue.empty());
		_alarm->arm(_timerQueue.top()->_deadline);
		current = _clock->currentNanos();
	} while(_timerQueue.top()->_deadline <= current);
}

ClockSource *systemClockSource() {
	return globalClockSource;
}

PrecisionTimerEngine *generalTimerEngine() {
	return globalTimerEngine;
}

} // namespace thor
