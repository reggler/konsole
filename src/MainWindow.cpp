/*
    Copyright 2006-2008 by Robert Knight <robertknight@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA.
*/

// Own
#include "MainWindow.h"
#include "SessionManager.h"

// Qt
#include <QtGui/QBoxLayout>

// KDE
#include <KAcceleratorManager>
#include <KAction>
#include <KActionCollection>
#include <KActionMenu>
#include <KApplication>
#include <KCmdLineArgs>
#include <KShortcutsDialog>
#include <KLocale>
#include <KMenu>
#include <KMenuBar>
#include <KMessageBox>
#include <KService>
#include <KToggleAction>
#include <KToggleFullScreenAction>
#include <KToolInvocation>
#include <KStandardAction>
#include <KStandardGuiItem>
#include <KWindowSystem>
#include <KXMLGUIFactory>
#include <KNotifyConfigWidget>
#include <KConfigDialog>

// Konsole
#include "BookmarkHandler.h"
#include "IncrementalSearchBar.h"
#include "RemoteConnectionDialog.h"
#include "SessionController.h"
#include "ProfileList.h"
#include "ManageProfilesDialog.h"
#include "Session.h"
#include "ViewManager.h"
#include "ViewSplitter.h"
#include "settings.h"
#include "settings/generalsettings.h"

using namespace Konsole;

static bool useTransparency()
{
    KCmdLineArgs* args = KCmdLineArgs::parsedArgs();
    bool compositingAvailable = KWindowSystem::compositingActive() ||
                                args->isSet("force-transparency");
    return compositingAvailable && args->isSet("transparency");
}

MainWindow::MainWindow()
 : KXmlGuiWindow() ,
   _bookmarkHandler(0),
   _pluggedController(0),
   _menuBarVisibilitySet(false)
{
    if (useTransparency()) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground, false);
    }

    // create actions for menus
    setupActions();

    // create view manager
        _viewManager = new ViewManager(this,actionCollection());
    connect( _viewManager , SIGNAL(empty()) , this , SLOT(close()) );
    connect( _viewManager , SIGNAL(activeViewChanged(SessionController*)) , this ,
            SLOT(activeViewChanged(SessionController*)) );
    connect( _viewManager , SIGNAL(unplugController(SessionController*)) , this ,
            SLOT(disconnectController(SessionController*)) );
    connect( _viewManager , SIGNAL(viewPropertiesChanged(const QList<ViewProperties*>&)) ,
           bookmarkHandler() , SLOT(setViews(const QList<ViewProperties*>&)) );

    connect( _viewManager , SIGNAL(setMenuBarVisibleRequest(bool)) , this ,
            SLOT(setMenuBarVisibleOnce(bool)) );
    connect( _viewManager , SIGNAL(newViewRequest(Profile::Ptr)) , 
        this , SLOT(newFromProfile(Profile::Ptr)) );
    connect( _viewManager , SIGNAL(newViewRequest()) , 
        this , SLOT(newTab()));

    // create main window widgets
    setupWidgets();

    // disable automatically generated accelerators in top-level
    // menu items - to avoid conflicting with Alt+[Letter] shortcuts
    // in terminal applications
    KAcceleratorManager::setNoAccel(menuBar());
    // create menus
    createGUI();
    // remove accelerators for standard menu items (eg. &File, &View, &Edit)
    // etc. which are defined in kdelibs/kdeui/xmlgui/ui_standards.rc, again,
    // to avoid conflicting with Alt+[Letter] terminal shortcuts
    //
    // TODO - Modify XMLGUI so that it allows the text for standard actions
    // defined in ui_standards.rc to be re-defined in the local application
    // XMLGUI file (konsoleui.rc in this case) - the text for standard items
    // can then be redefined there to exclude the standard accelerators
    removeMenuAccelerators();
    // replace standard shortcuts which cannot be used in a terminal
    // (as they are reserved for use by terminal programs)
    correctShortcuts();

    // enable save and restore of window size
    setAutoSaveSettings("Window", Settings::saveGeometryOnExit());
    kDebug()<< Settings::saveGeometryOnExit();
}
void MainWindow::removeMenuAccelerators()
{
    foreach(QAction* menuItem, menuBar()->actions())
    {
        QString itemText = menuItem->text();
        itemText = KGlobal::locale()->removeAcceleratorMarker(itemText);
        menuItem->setText(itemText);
    }
}
void MainWindow::setMenuBarVisibleOnce(bool visible)
{
    if (_menuBarVisibilitySet || menuBar()->isTopLevelMenu() )
        return;

    menuBar()->setVisible(visible);
    _toggleMenuBarAction->setChecked(visible);

    _menuBarVisibilitySet = true;
}

void MainWindow::setSaveGeometryOnExit(bool save)
{
    setAutoSaveSettings("Window",save);
}

void MainWindow::correctShortcuts()
{
    // replace F1 shortcut for help contents
    QAction* helpAction = actionCollection()->action("help_contents");

    Q_ASSERT( helpAction );

    helpAction->setShortcut(QKeySequence());
   
    // replace Ctrl+B shortcut for bookmarks
    // TODO - Make this configurable
    QAction* bookmarkAction = actionCollection()->action("add_bookmark");
    Q_ASSERT(bookmarkAction);
    bookmarkAction->setShortcut(QKeySequence(Qt::CTRL+Qt::SHIFT+Qt::Key_B));
}

void MainWindow::setDefaultProfile(Profile::Ptr profile)
{
    _defaultProfile = profile;
}
Profile::Ptr MainWindow::defaultProfile() const
{
    return _defaultProfile;
}

ViewManager* MainWindow::viewManager() const
{
    return _viewManager;
}

void MainWindow::disconnectController(SessionController* controller)
{
    disconnect( controller , SIGNAL(titleChanged(ViewProperties*))
                     , this , SLOT(activeViewTitleChanged(ViewProperties*)) );

    // KXmlGuiFactory::removeClient() will try to access actions associated
    // with the controller internally, which may not be valid after the controller
    // itself is no longer valid (after the associated session and or view have
    // been destroyed)
    if (controller->isValid())
        guiFactory()->removeClient(controller);

    controller->setSearchBar(0);
}

void MainWindow::activeViewChanged(SessionController* controller)
{
    // associate bookmark menu with current session
    bookmarkHandler()->setActiveView(controller);
    disconnect( bookmarkHandler() , SIGNAL(openUrl(const KUrl&)) , 0 , 0 );
    connect( bookmarkHandler() , SIGNAL(openUrl(const KUrl&)) , controller ,
             SLOT(openUrl(const KUrl&)) );

    if ( _pluggedController )
        disconnectController(_pluggedController);

    // listen for title changes from the current session
    Q_ASSERT( controller );

    connect( controller , SIGNAL(titleChanged(ViewProperties*)) ,
            this , SLOT(activeViewTitleChanged(ViewProperties*)) );

    controller->setShowMenuAction( _toggleMenuBarAction );
    guiFactory()->addClient(controller);

    // set the current session's search bar
    controller->setSearchBar( searchBar() );

    // update session title to match newly activated session
    activeViewTitleChanged(controller);

    _pluggedController = controller;
}

void MainWindow::activeViewTitleChanged(ViewProperties* properties)
{
    setPlainCaption(properties->title());
}

IncrementalSearchBar* MainWindow::searchBar() const
{
    return _viewManager->searchBar();
}

void MainWindow::setupActions()
{
    KActionCollection* collection = actionCollection();

    // File Menu
    _newTabMenuAction = new KActionMenu(KIcon("tab-new"), i18n("&New Tab"), collection);
    _newTabMenuAction->setShortcut( QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_T) );
    _newTabMenuAction->setShortcutConfigurable(true);
    _newTabMenuAction->setAutoRepeat( false );
    connect(_newTabMenuAction, SIGNAL(triggered()), this, SLOT(newTab()));
    collection->addAction("new-tab", _newTabMenuAction);

    KAction* action = collection->addAction("new-window");
    action->setIcon( KIcon("window-new") );
    action->setText( i18n("New &Window") );
    action->setShortcut( QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_N) );
    action->setAutoRepeat( false );
    connect( action , SIGNAL(triggered()) , this , SLOT(newWindow()) );

    action = collection->addAction("remote-connection");
    action->setText( i18n("Remote Connection...") );
    action->setIcon( KIcon("network-connect") );
    action->setShortcut( QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_R) );
    connect( action , SIGNAL(triggered()) , this , SLOT(showRemoteConnectionDialog()) );

    action = KStandardAction::quit( this , SLOT(close()) , collection );
    // the default shortcut for quit is typically Ctrl+[Some Letter, usually Q] but that is reserved for
    // use by terminal applications
    action->setShortcut( QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_Q) );

    // Bookmark Menu
    KActionMenu* bookmarkMenu = new KActionMenu(i18n("&Bookmarks") , collection );
    _bookmarkHandler = new BookmarkHandler( collection , bookmarkMenu->menu() , true , this );
    collection->addAction("bookmark" , bookmarkMenu);

    connect( _bookmarkHandler , SIGNAL(openUrls(QList<KUrl>)) , this , SLOT(openUrls(QList<KUrl>)) );

    //TODO: The 'Add Bookmark' menu action currently has a Ctrl+B shortcut by
    //      default which cannot be overridden
    //NOTE: This is currently handled by correctShortcuts()

    // View Menu
    _toggleMenuBarAction = KStandardAction::showMenubar(menuBar(), SLOT(setVisible(bool)), collection);
    _toggleMenuBarAction->setShortcut( QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_M) );

    // Hide the Show/Hide menubar item if the menu bar is a MacOS-style menu bar
    if ( menuBar()->isTopLevelMenu() )
        _toggleMenuBarAction->setVisible(false);

    // Full Screen
    action = KStandardAction::fullScreen(this, SLOT(viewFullScreen(bool)), this, collection);
    action->setShortcut( QKeySequence() );

    // Settings Menu
    KStandardAction::configureNotifications( this , SLOT(configureNotifications()) , collection  );
    KStandardAction::keyBindings( this , SLOT(showShortcutsDialog()) , collection  );
    KStandardAction::preferences(this, SLOT(showSettingsDialog()), collection);

    action = collection->addAction("configure-profiles");
    action->setText( i18n("Configure Profiles...") );
    action->setIcon( KIcon("configure") );
    connect( action, SIGNAL(triggered()) , this , SLOT(showManageProfilesDialog()) );

}

void MainWindow::viewFullScreen(bool fullScreen)
{
    if ( fullScreen )
        setWindowState( windowState() | Qt::WindowFullScreen );
    else
        setWindowState( windowState() & ~Qt::WindowFullScreen );
}

BookmarkHandler* MainWindow::bookmarkHandler() const
{
    return _bookmarkHandler;
}

void MainWindow::setSessionList(ProfileList* list)
{
    sessionListChanged(list->actions());

    connect( list , SIGNAL(profileSelected(Profile::Ptr)) , this ,
            SLOT(newFromProfile(Profile::Ptr)) );

    connect( list , SIGNAL(actionsChanged(const QList<QAction*>&)) , this ,
            SLOT(sessionListChanged(const QList<QAction*>&)) );
}

void MainWindow::sessionListChanged(const QList<QAction*>& actions)
{
    // Update the 'New Tab' KActionMenu
    KMenu *newTabMenu = _newTabMenuAction->menu();
    newTabMenu->clear();
    foreach (QAction *action, actions) {
        newTabMenu->addAction(action);

        // NOTE: _defaultProfile seems to not work here, sigh.
        Profile::Ptr profile = SessionManager::instance()->defaultProfile();
        if (profile && profile->name() == action->text()) {
            action->setIcon(KIcon(profile->icon(), NULL, QStringList("emblem-favorite")));
            newTabMenu->setDefaultAction(action);
            QFont font = action->font();
            font.setBold(true);
            action->setFont(font);
        }
    }

}

QString MainWindow::activeSessionDir() const
{
    if ( _pluggedController )
    {
        if ( Session* session = _pluggedController->session() )
        {
            // For new tabs to get the correct working directory,
            // force the updating of the currentWorkingDirectory.
            session->getDynamicTitle();
        }
        return _pluggedController->currentDir();
    }
    else
    {
        return QString();
    }
}

void MainWindow::openUrls(const QList<KUrl>& urls)
{
    foreach( const KUrl& url , urls )
    {
        if ( url.isLocalFile() )
            emit newSessionRequest( _defaultProfile , url.path() , _viewManager );

        else if ( url.protocol() == "ssh" )
            emit newSSHSessionRequest( _defaultProfile , url , _viewManager );
    }
}

void MainWindow::newTab()
{
    emit newSessionRequest( _defaultProfile , activeSessionDir() , _viewManager);
}

void MainWindow::newWindow()
{
    emit newWindowRequest( _defaultProfile , activeSessionDir() );
}

bool MainWindow::queryClose()
{
    if (kapp->sessionSaving() ||
        _viewManager->viewProperties().count() < 2)
        return true;

    int result = KMessageBox::warningYesNoCancel(this,
                i18n("You have multiple tabs in this window, "
                     "are you sure you want to quit?"),
                i18n("Confirm Close"),
                KStandardGuiItem::quit(),
                KGuiItem(i18n("Close Current Tab"), "tab-close"),
                KStandardGuiItem::cancel(),
                "CloseAllTabs");

    switch (result)
    {
    case KMessageBox::Yes:
        return true;
    case KMessageBox::No:
        if (_pluggedController && _pluggedController->session())
        {
            disconnectController(_pluggedController);
            _pluggedController->session()->close();
        }
        return false;
    case KMessageBox::Cancel:
        return false;
    }

    return true;
}

void MainWindow::saveProperties(KConfigGroup& group)
{
    if (_defaultProfile)
        group.writePathEntry("Default Profile", _defaultProfile->path());
    _viewManager->saveSessions(group);
}

void MainWindow::readProperties(const KConfigGroup& group)
{
    SessionManager *manager = SessionManager::instance();
    QString profilePath = group.readPathEntry("Default Profile", QString());
    Profile::Ptr profile = manager->defaultProfile();
    if (!profilePath.isEmpty()) 
        profile = manager->loadProfile(profilePath);
    setDefaultProfile(profile);
    _viewManager->restoreSessions(group);
}

void MainWindow::saveGlobalProperties(KConfig* config)
{
    SessionManager::instance()->saveSessions(config);
}

void MainWindow::readGlobalProperties(KConfig* config)
{
    SessionManager::instance()->restoreSessions(config);
}

void MainWindow::syncActiveShortcuts(KActionCollection* dest, const KActionCollection* source)
{
    foreach(QAction* qAction, source->actions()) 
    {
        if (KAction* kAction = qobject_cast<KAction*>(qAction))
        {
           if (KAction* destKAction = qobject_cast<KAction*>(dest->action(kAction->objectName())))
               destKAction->setShortcut(kAction->shortcut(KAction::ActiveShortcut),KAction::ActiveShortcut);
        }
    }
}
void MainWindow::showShortcutsDialog()
{
    KShortcutsDialog dialog(KShortcutsEditor::AllActions, KShortcutsEditor::LetterShortcutsDisallowed, this);

    // add actions from this window and the current session controller
    foreach(KXMLGUIClient* client, guiFactory()->clients())
                dialog.addCollection(client->actionCollection());

    if (dialog.configure())
    {
        // sync shortcuts for non-session actions (defined in "konsoleui.rc") in other main windows
        foreach(QWidget* widget, QApplication::topLevelWidgets())
        {
            MainWindow* window = qobject_cast<MainWindow*>(widget);
            if (window && window != this)
                syncActiveShortcuts(window->actionCollection(),actionCollection());
        }
        // sync shortcuts for session actions (defined in "sessionui.rc") in other session controllers.
        // Controllers which are currently plugged in (ie. their actions are part of the current menu)
        // must be updated immediately via syncActiveShortcuts().  Other controllers will be updated
        // when they are plugged into a main window.
        foreach(SessionController* controller, SessionController::allControllers())
        {
            controller->reloadXML();
            if (controller->factory() && controller != _pluggedController)
                syncActiveShortcuts(controller->actionCollection(),_pluggedController->actionCollection());
        }
    }
}

void MainWindow::newFromProfile(Profile::Ptr profile)
{
    emit newSessionRequest(profile, activeSessionDir(), _viewManager);
}
void MainWindow::showManageProfilesDialog()
{
    ManageProfilesDialog* dialog = new ManageProfilesDialog(this);
    dialog->show();
}

void MainWindow::showSettingsDialog()
{
    if (KConfigDialog::showDialog("settings"))
        return;

    KConfigDialog* settingsDialog = new KConfigDialog(this, "settings",
                                                      Settings::self());
    settingsDialog->setFaceType(KPageDialog::List);

    connect(settingsDialog, SIGNAL(settingsChanged(const QString&)),
            this, SLOT(applySettings()));

    GeneralSettings* generalSettings = new GeneralSettings(settingsDialog);
    settingsDialog->addPage(generalSettings, 
                            i18nc("@title Preferences page name", "General"),
                            "system-run");

    settingsDialog->show();
}

void MainWindow::showRemoteConnectionDialog()
{
//    RemoteConnectionDialog dialog(this);
//    if ( dialog.exec() == QDialog::Accepted )
//        emit newSessionRequest(dialog.sessionKey(),QString(),_viewManager);
}

void MainWindow::setupWidgets()
{
    QWidget* widget = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout();

    layout->addWidget( _viewManager->widget() );
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    widget->setLayout(layout);

    setCentralWidget(widget);
}

void MainWindow::configureNotifications()
{
    KNotifyConfigWidget::configure( this );
}

void MainWindow::showEvent(QShowEvent *event)
{
    // This code from Konqueror.
    // We need to check if our toolbars are shown/hidden here, and set
    // our menu items accordingly. We can't do it in the constructor because
    // view profiles store toolbar info, and that info is read after
    // construct time.
    _toggleMenuBarAction->setChecked( !menuBar()->isHidden() );
    // Call parent method
    KXmlGuiWindow::showEvent(event);
}

void MainWindow::applySettings()
{
    setAutoSaveSettings("Window", Settings::saveGeometryOnExit());
    kDebug()<< Settings::saveGeometryOnExit();
}

#include "MainWindow.moc"

