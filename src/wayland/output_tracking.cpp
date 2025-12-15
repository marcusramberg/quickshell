#include "output_tracking.hpp"

#include <private/qwaylanddisplay_p.h>
#include <private/qwaylandintegration_p.h>
#include <private/qwaylandscreen_p.h>
#include <qguiapplication.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qscreen.h>
#include <qtmetamacros.h>

// NOTE: There is a Qt bug where wl_output pointers can become stale/dangling when
// a monitor is disconnected. Qt receives surface_enter events with these invalid
// pointers and crashes in wl_proxy_get_listener(). This manifests as crashes in
// QWaylandSurface::surface_enter() -> QWaylandScreen::fromWlOutput().
// We add defensive checks here, but Qt's internal window handling is still vulnerable.
// See: https://bugreports.qt.io/ (TODO: file bug report)

namespace qs::wayland {

void WlOutputTracker::addOutput(::wl_output* output) {
	// Null output can happen during compositor shutdown or errors
	if (output == nullptr) return;
	
	auto* display = QtWaylandClient::QWaylandIntegration::instance()->display();
	if (display == nullptr) return;
	
	auto* guiApp = static_cast<QGuiApplication*>(QGuiApplication::instance()); // NOLINT
	
	// Always connect to screenRemoved to catch Qt destroying screens
	QObject::connect(
	    guiApp,
	    &QGuiApplication::screenRemoved,
	    this,
	    &WlOutputTracker::onQScreenRemoved,
	    Qt::UniqueConnection
	);

	// The output pointer might be stale if it was destroyed by the compositor
	// before Qt processed the event. screenForOutput() can crash on invalid outputs.
	QtWaylandClient::QWaylandScreen* platformScreen = nullptr;
	try {
		platformScreen = display->screenForOutput(output);
	} catch (...) {
		// Invalid output pointer - ignore this output
		return;
	}
	
	if (platformScreen != nullptr) {
		auto* screen = platformScreen->screen();
		this->mScreens.append(screen);
		emit this->screenAdded(screen);
	} else {
		QObject::connect(
		    guiApp,
		    &QGuiApplication::screenAdded,
		    this,
		    &WlOutputTracker::onQScreenAdded,
		    Qt::UniqueConnection
		);

		this->mOutputs.append(output);
	}
}

void WlOutputTracker::removeOutput(::wl_output* output) {
	// Null output can happen during compositor shutdown or errors
	if (output == nullptr) return;
	
	auto* display = QtWaylandClient::QWaylandIntegration::instance()->display();
	if (display == nullptr) return;

	// The output pointer might be stale if it was destroyed by the compositor
	// before Qt processed the event. screenForOutput() can crash on invalid outputs.
	QtWaylandClient::QWaylandScreen* platformScreen = nullptr;
	try {
		platformScreen = display->screenForOutput(output);
	} catch (...) {
		// Invalid output pointer - try to clean up from mOutputs
		this->mOutputs.removeOne(output);
		return;
	}
	
	if (platformScreen != nullptr) {
		auto* screen = platformScreen->screen();
		this->mScreens.removeOne(screen);
		emit this->screenRemoved(screen);
	} else {
		// Platform screen not found - either already removed by Qt or pending addition.
		// If it's in mOutputs, it was pending and we should clean that up.
		this->mOutputs.removeOne(output);

		if (this->mOutputs.isEmpty()) {
			QObject::disconnect(
			    static_cast<QGuiApplication*>(QGuiApplication::instance()), // NOLINT
			    nullptr,
			    this,
			    nullptr
			);
		}
	}
}

void WlOutputTracker::onQScreenAdded(QScreen* screen) {
	if (auto* platformScreen = dynamic_cast<QtWaylandClient::QWaylandScreen*>(screen->handle())) {
		if (this->mOutputs.removeOne(platformScreen->output())) {
			this->mScreens.append(screen);
			emit this->screenAdded(screen);

			if (this->mOutputs.isEmpty()) {
				QObject::disconnect(
				    static_cast<QGuiApplication*>(QGuiApplication::instance()), // NOLINT
				    nullptr,
				    this,
				    nullptr
				);
			}
		}
	}
}

void WlOutputTracker::onQScreenRemoved(QScreen* screen) {
	// Qt is destroying this screen, so remove it from our tracking list
	// to avoid dangling pointers. We don't emit screenRemoved here because
	// the screen pointer may be invalid by the time slots are called.
	// Slots should already be notified via the Wayland output removal path.
	this->mScreens.removeOne(screen);
}

} // namespace qs::wayland
