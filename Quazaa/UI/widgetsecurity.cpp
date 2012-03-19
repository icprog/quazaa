/*
** $Id$
**
** Copyright © Quazaa Development Team, 2009-2012.
** This file is part of QUAZAA (quazaa.sourceforge.net)
**
** Quazaa is free software; this file may be used under the terms of the GNU
** General Public License version 3.0 or later as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.
**
** Quazaa is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
**
** Please review the following information to ensure the GNU General Public
** License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** You should have received a copy of the GNU General Public License version
** 3.0 along with Quazaa; if not, write to the Free Software Foundation,
** Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "securitytablemodel.h"

#include "widgetsecurity.h"
#include "ui_widgetsecurity.h"
#include "dialogaddrule.h"
#include "dialogsecuritysubscriptions.h"

#include "quazaasettings.h"
#include "skinsettings.h"

#include "timedsignalqueue.h"

#ifdef _DEBUG
#include "debug_new.h"
#endif

WidgetSecurity::WidgetSecurity(QWidget* parent) :
	QMainWindow( parent ),
	ui( new Ui::WidgetSecurity )
{
	ui->setupUi( this );

	securityMenu = new QMenu(ui->tableViewSecurity);
	securityMenu->addAction(ui->actionSecurityModifyRule);
	securityMenu->addAction(ui->actionSecurityRemoveRule);
	securityMenu->addAction(ui->actionSecurityExportRules);

	restoreState( quazaaSettings.WinMain.SecurityToolbars );

	m_pSecurityList = new CSecurityTableModel( this, tableView() );
	setModel( m_pSecurityList );
	m_pSecurityList->sort( ui->tableViewSecurity->horizontalHeader()->sortIndicatorSection(),
						   ui->tableViewSecurity->horizontalHeader()->sortIndicatorOrder() );
	setSkin();
}

WidgetSecurity::~WidgetSecurity()
{
	delete ui;
}

void WidgetSecurity::setModel(QAbstractItemModel* model)
{
	ui->tableViewSecurity->setModel( model );
}

QWidget* WidgetSecurity::tableView()
{
	return ui->tableViewSecurity;
}

void WidgetSecurity::saveWidget()
{
	quazaaSettings.WinMain.SecurityToolbars = saveState();
}

void WidgetSecurity::changeEvent(QEvent* e)
{
	QMainWindow::changeEvent( e );
	switch ( e->type() )
	{
	case QEvent::LanguageChange:
		ui->retranslateUi( this );
		break;
	default:
		break;
	}
}

void WidgetSecurity::keyPressEvent(QKeyEvent *e)
{
	switch ( e->key() )
	{
	case Qt::Key_Delete:
		on_actionSecurityRemoveRule_triggered();
	}

	QMainWindow::keyPressEvent( e );
}

void WidgetSecurity::update()
{
	m_pSecurityList->updateAll();
}

void WidgetSecurity::on_actionSecurityAddRule_triggered()
{
	DialogAddRule* dlgAddRule = new DialogAddRule( this );
	connect( dlgAddRule, SIGNAL( dataUpdated() ), SLOT( update() ), Qt::QueuedConnection );
	dlgAddRule->show();
}

void WidgetSecurity::on_actionSecurityRemoveRule_triggered()
{
	QModelIndex index = ui->tableViewSecurity->currentIndex();

	if ( index.isValid() )
	{
		// Lock security manager while fiddling with rule.
		QReadLocker l( &securityManager.m_pRWLock );
		security::CSecureRule* pRule = m_pSecurityList->nodeFromIndex( index );
		securityManager.remove( pRule, false );
	}
}

void WidgetSecurity::on_actionSecurityModifyRule_triggered()
{
	QModelIndex index = ui->tableViewSecurity->currentIndex();

	if ( index.isValid() )
	{
		// Lock security manager while fiddling with rule.
		QReadLocker lock( &securityManager.m_pRWLock );
		security::CSecureRule* pRule = m_pSecurityList->nodeFromIndex( index );
		DialogAddRule* dlgAddRule = new DialogAddRule( this, pRule );
		lock.unlock();

		connect( dlgAddRule, SIGNAL( dataUpdated() ), SLOT( update() ), Qt::QueuedConnection );
		dlgAddRule->show();
	}
}

void WidgetSecurity::on_actionSecurityImportRules_triggered()
{

}

void WidgetSecurity::on_actionSecurityExportRules_triggered()
{

}

void WidgetSecurity::on_actionSubscribeSecurityList_triggered()
{
	DialogSecuritySubscriptions* dlgSecuritySubscriptions = new DialogSecuritySubscriptions( this );
	dlgSecuritySubscriptions->show();
}

void WidgetSecurity::on_tableViewSecurity_customContextMenuRequested(const QPoint& point)
{
	QModelIndex index = ui->tableViewSecurity->indexAt( point );

	if ( index.isValid() )
	{
		ui->actionSecurityExportRules->setEnabled( true );
		ui->actionSecurityModifyRule->setEnabled( true );
		ui->actionSecurityRemoveRule->setEnabled( true );
	}
	else
	{
		ui->actionSecurityExportRules->setEnabled( false );
		ui->actionSecurityModifyRule->setEnabled( false );
		ui->actionSecurityRemoveRule->setEnabled( false );
	}

	securityMenu->popup( QCursor::pos() );
}

void WidgetSecurity::on_tableViewSecurity_doubleClicked(const QModelIndex& index)
{
	if ( index.isValid() )
	{
		on_actionSecurityModifyRule_triggered();
	}
	else
	{
		ui->actionSecurityExportRules->setEnabled( false );
		ui->actionSecurityModifyRule->setEnabled( false );
		ui->actionSecurityRemoveRule->setEnabled( false );
	}
}

void WidgetSecurity::on_tableViewSecurity_clicked(const QModelIndex& index)
{
	if ( index.isValid() )
	{
		ui->actionSecurityExportRules->setEnabled( true );
		ui->actionSecurityModifyRule->setEnabled( true );
		ui->actionSecurityRemoveRule->setEnabled( true );
	}
	else
	{
		ui->actionSecurityExportRules->setEnabled( false );
		ui->actionSecurityModifyRule->setEnabled( false );
		ui->actionSecurityRemoveRule->setEnabled( false );
	}
}

void WidgetSecurity::setSkin()
{
	ui->tableViewSecurity->setStyleSheet(skinSettings.listViews);
}
