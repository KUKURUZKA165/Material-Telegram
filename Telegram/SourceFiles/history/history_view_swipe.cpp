/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/history_view_swipe.h"

#include "base/platform/base_platform_haptic.h"
#include "base/platform/base_platform_info.h"
#include "base/qt/qt_common_adapters.h"
#include "base/event_filter.h"
#include "history/history_view_swipe_data.h"
#include "ui/chat/chat_style.h"
#include "ui/ui_utility.h"
#include "ui/widgets/elastic_scroll.h"
#include "ui/widgets/scroll_area.h"

#include <QtWidgets/QApplication>

namespace HistoryView {

void SetupSwipeHandler(
		not_null<Ui::RpWidget*> widget,
		not_null<Ui::ScrollArea*> scroll,
		Fn<void(ChatPaintGestureHorizontalData)> update,
		Fn<SwipeHandlerFinishData(int)> generateFinishByTop) {
	constexpr auto kThresholdWidth = 50;
	const auto threshold = style::ConvertFloatScale(kThresholdWidth);
	struct State {
		base::unique_qptr<QObject> filter;
		Ui::Animations::Simple animationReach;
		Ui::Animations::Simple animationEnd;
		SwipeHandlerFinishData finishByTopData;
		std::optional<Qt::Orientation> orientation;
		QPointF startAt;
		QPointF delta;
		int cursorTop = 0;
		bool started = false;
		bool reached = false;
		bool touch = false;

		rpl::lifetime lifetime;
	};
	const auto state = widget->lifetime().make_state<State>();
	const auto updateRatio = [=](float64 ratio) {
		update({
			.ratio = std::clamp(ratio, 0., 1.5),
			.reachRatio = state->animationReach.value(0.),
			.translation = int(
				base::SafeRound(-std::clamp(ratio, 0., 1.5) * threshold)),
			.msgBareId = state->finishByTopData.msgBareId,
			.cursorTop = state->cursorTop,
		});
	};
	const auto setOrientation = [=](std::optional<Qt::Orientation> o) {
		state->orientation = o;
		const auto isHorizontal = (o == Qt::Horizontal);
		scroll->viewport()->setAttribute(
			Qt::WA_AcceptTouchEvents,
			!isHorizontal);
		scroll->disableScroll(isHorizontal);
	};
	const auto processEnd = [=](std::optional<QPointF> delta = {}) {
		if (state->orientation == Qt::Horizontal) {
			const auto ratio = delta.value_or(state->delta).x() / threshold;
			if ((ratio >= 1) && state->finishByTopData.callback) {
				Ui::PostponeCall(
					widget,
					state->finishByTopData.callback);
			}
			state->animationReach.stop();
			state->animationEnd.stop();
			state->animationEnd.start(
				updateRatio,
				ratio,
				0.,
				st::slideWrapDuration);
		}
		setOrientation(std::nullopt);
		state->started = false;
		state->reached = false;
	};
	scroll->scrolls() | rpl::start_with_next([=] {
		processEnd();
	}, state->lifetime);
	const auto animationReachCallback = [=] {
		updateRatio(state->delta.x() / threshold);
	};
	struct UpdateArgs {
		QPointF position;
		QPointF delta;
		bool touch = false;
	};
	const auto updateWith = [=](UpdateArgs &&args) {
		if (!state->started || state->touch != args.touch) {
			state->started = true;
			state->touch = args.touch;
			state->startAt = args.position;
			state->delta = QPointF();
			state->cursorTop = widget->mapFromGlobal(
				QCursor::pos()).y();
			state->finishByTopData = generateFinishByTop(
				state->cursorTop);
			if (!state->finishByTopData.callback) {
				setOrientation(Qt::Vertical);
			}
		} else if (!state->orientation) {
			state->delta = args.delta;
			const auto diffXtoY = std::abs(args.delta.x())
				- std::abs(args.delta.y());
			constexpr auto kOrientationThreshold = 1.;
			if (diffXtoY > kOrientationThreshold) {
				setOrientation(Qt::Horizontal);
			} else if (diffXtoY < -kOrientationThreshold) {
				setOrientation(Qt::Vertical);
			} else {
				setOrientation(std::nullopt);
			}
		} else if (*state->orientation == Qt::Horizontal) {
			state->delta = args.delta;
			const auto ratio = args.delta.x() / threshold;
			updateRatio(ratio);
			constexpr auto kResetReachedOn = 0.95;
			constexpr auto kBounceDuration = crl::time(500);
			if (!state->reached && ratio >= 1.) {
				state->reached = true;
				state->animationReach.stop();
				state->animationReach.start(
					animationReachCallback,
					0.,
					1.,
					kBounceDuration);
				base::Platform::Haptic();
			} else if (state->reached
				&& ratio < kResetReachedOn) {
				state->reached = false;
			}
		}
	};
	const auto filter = [=](not_null<QEvent*> e) {
		const auto type = e->type();
		switch (type) {
		case QEvent::Leave: {
			if (state->orientation) {
				processEnd();
			}
		} break;
		case QEvent::MouseMove: {
			if (state->orientation) {
				const auto m = static_cast<QMouseEvent*>(e.get());
				if (std::abs(m->pos().y() - state->cursorTop)
					> QApplication::startDragDistance()) {
					processEnd();
				}
			}
		} break;
		case QEvent::TouchBegin:
		case QEvent::TouchUpdate:
		case QEvent::TouchEnd:
		case QEvent::TouchCancel: {
			const auto t = static_cast<QTouchEvent*>(e.get());
			const auto touchscreen = t->device()
				&& (t->device()->type() == base::TouchDevice::TouchScreen);
			if (!Platform::IsMac() && !touchscreen) {
				break;
			}
			const auto &touches = t->touchPoints();
			const auto released = [&](int index) {
				return (touches.size() > index)
					&& (touches.at(index).state() & Qt::TouchPointReleased);
			};
			const auto cancel = released(0)
				|| released(1)
				|| (touchscreen
					? (touches.size() != 1)
					: (touches.size() <= 0 || touches.size() > 2))
				|| (type == QEvent::TouchEnd)
				|| (type == QEvent::TouchCancel);
			if (cancel) {
				processEnd(touches.empty()
					? std::optional<QPointF>()
					: (state->startAt - touches[0].pos()));
			} else {
				updateWith({
					.position = touches[0].pos(),
					.delta = state->startAt - touches[0].pos(),
					.touch = true,
				});
			}
			return base::EventFilterResult::Cancel;
		} break;
		case QEvent::Wheel: {
			const auto w = static_cast<QWheelEvent*>(e.get());
			const auto phase = w->phase();
			if (Platform::IsMac() || phase == Qt::NoScrollPhase) {
				break;
			}
			const auto cancel = w->buttons()
				|| (phase == Qt::ScrollEnd)
				|| (phase == Qt::ScrollMomentum);
			if (cancel) {
				processEnd();
			} else {
				updateWith({
					.position = QPointF(),
					.delta = state->delta - Ui::ScrollDelta(w),
					.touch = false,
				});
			}
		} break;
		}
		return base::EventFilterResult::Continue;
	};
	state->filter = base::make_unique_q<QObject>(
		base::install_event_filter(widget, filter));
}

} // namespace HistoryView
