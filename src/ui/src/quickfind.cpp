/*
 * Copyright (C) 2010, 2013 Nicolas Bonnefon and other contributors
 *
 * This file is part of glogg.
 *
 * glogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * glogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with glogg.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (C) 2016 -- 2019 Anton Filimonov and other contributors
 *
 * This file is part of klogg.
 *
 * klogg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * klogg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with klogg.  If not, see <http://www.gnu.org/licenses/>.
 */

// This file implements QuickFind.
// This class implements the Quick Find mechanism using references
// to the logData, the QFP and the selection passed.
// Search is started just after the selection and the selection is updated
// if a match is found.

#include <QApplication>
#include <QtConcurrent>
#include <QtPromise>

#include "data/abstractlogdata.h"
#include "log.h"
#include "quickfindpattern.h"
#include "selection.h"

#include "quickfind.h"

void SearchingNotifier::reset()
{
    dotToDisplay_ = 0;
    startTime_ = QTime::currentTime();
}

void SearchingNotifier::sendNotification( qint64 current_line, qint64 nb_lines )
{
    LOG( logDEBUG ) << "Emitting Searching....";
    qint64 progress;
    if ( current_line < 0 )
        progress = ( nb_lines + current_line ) * 100 / nb_lines;
    else
        progress = current_line * 100 / nb_lines;
    emit notify( QFNotificationProgress( progress ) );

    QApplication::processEvents( QEventLoop::ExcludeUserInputEvents );
    startTime_ = QTime::currentTime().addMSecs( -800 );
}

void QuickFind::LastMatchPosition::set( LineNumber line, int column )
{
    if ( ( !line_.has_value() ) || ( ( line <= *line_ ) && ( column < column_ ) ) ) {
        line_ = line;
        column_ = column;
    }
}

void QuickFind::LastMatchPosition::set( const FilePosition& position )
{
    set( position.line(), position.column() );
}

bool QuickFind::LastMatchPosition::isLater( OptionalLineNumber line, int column ) const
{
    if ( !line_.has_value() || !line.has_value() )
        return false;
    else if ( ( *line == *line_ ) && ( column >= column_ ) )
        return true;
    else if ( *line > *line_ )
        return true;
    else
        return false;
}

bool QuickFind::LastMatchPosition::isLater( const FilePosition& position ) const
{
    return isLater( position.line(), position.column() );
}

bool QuickFind::LastMatchPosition::isSooner( OptionalLineNumber line, int column ) const
{
    if ( !line_.has_value() || !line.has_value() )
        return false;
    else if ( ( *line == *line_ ) && ( column <= column_ ) )
        return true;
    else if ( *line < *line_ )
        return true;
    else
        return false;
}

bool QuickFind::LastMatchPosition::isSooner( const FilePosition& position ) const
{
    return isSooner( position.line(), position.column() );
}

QuickFind::QuickFind( const AbstractLogData& logData )
    : logData_( logData )
    , searchingNotifier_()
    , incrementalSearchStatus_()
    , searchPromise_{ new QtPromise::QPromise<void>( QtPromise::resolve() ) }
{
    connect( &searchingNotifier_, &SearchingNotifier::notify, this, &QuickFind::sendNotification,
             Qt::DirectConnection );
}

Selection QuickFind::incrementalSearchStop()
{
    if ( incrementalSearchStatus_.isOngoing() ) {
        Selection s = incrementalSearchStatus_.initialSelection();
        incrementalSearchStatus_ = IncrementalSearchStatus();
        interruptRequested_.set();

        return s;
    }
    else {
        return Selection{};
    }
}

Selection QuickFind::incrementalSearchAbort()
{
    if ( incrementalSearchStatus_.isOngoing() ) {
        Selection s = incrementalSearchStatus_.initialSelection();
        incrementalSearchStatus_ = IncrementalSearchStatus();
        interruptRequested_.set();
        return s;
    }
    else {
        return Selection{};
    }
}

void QuickFind::stopSearch()
{
    LOG( logINFO ) << "Stop search for quickfind " << this;
    interruptRequested_.set();
    searchPromise_->wait();
}

void QuickFind::incrementallySearchForward( Selection selection, QuickFindMatcher matcher )
{
    LOG( logDEBUG ) << "QuickFind::incrementallySearchForward";

    interruptRequested_.set();
    searchPromise_->wait();

    // Position where we start the search from
    FilePosition start_position = selection.getNextPosition();

    if ( incrementalSearchStatus_.direction() == Forward ) {
        // An incremental search is active, we restart the search
        // from the initial point
        LOG( logDEBUG ) << "Restart search from initial point";
        start_position = incrementalSearchStatus_.position();
    }
    else {
        // It's a new search so we search from the selection
        incrementalSearchStatus_ = IncrementalSearchStatus( Forward, start_position, selection );
    }

    searchPromise_ = std::make_unique<QtPromise::QPromise<void>>(
        QtPromise::resolve( QtConcurrent::run( this, &QuickFind::doSearchForward, start_position,
                                               selection, matcher ) )
            .then( [this]( Portion line_found ) {
                if ( line_found.isValid() ) {
                    // We have found a result...
                    // ... the caller will jump to this line.
                    emit searchDone( true, line_found );
                }
                else {
                    // No result...
                    // ... we want the client to show the initial line.
                    emit searchDone( false,
                                     Portion{ incrementalSearchStatus_.position().line(), 0, 0 } );
                }
            } ) );
}

void QuickFind::incrementallySearchBackward( Selection selection, QuickFindMatcher matcher )
{
    LOG( logDEBUG ) << "QuickFind::incrementallySearchBackward";

    interruptRequested_.set();
    searchPromise_->wait();

    // Position where we start the search from
    FilePosition start_position = selection.getPreviousPosition();

    if ( incrementalSearchStatus_.direction() == Backward ) {
        // An incremental search is active, we restart the search
        // from the initial point
        LOG( logDEBUG ) << "Restart search from initial point";
        start_position = incrementalSearchStatus_.position();
    }
    else {
        // It's a new search so we search from the selection
        incrementalSearchStatus_ = IncrementalSearchStatus( Backward, start_position, selection );
    }

    searchPromise_ = std::make_unique<QtPromise::QPromise<void>>(
        QtPromise::resolve( QtConcurrent::run( this, &QuickFind::doSearchBackward, start_position,
                                               selection, matcher ) )
            .then( [this]( Portion line_found ) {
                if ( line_found.isValid() ) {
                    // We have found a result...
                    // ... the caller will jump to this line.
                    emit searchDone( true, line_found );
                }
                else {
                    // No result...
                    // ... we want the client to show the initial line.
                    emit searchDone( false,
                                     Portion{ incrementalSearchStatus_.position().line(), 0, 0 } );
                }
            } ) );
}

void QuickFind::searchForward( Selection selection, QuickFindMatcher matcher )
{
    incrementalSearchStatus_ = IncrementalSearchStatus();
    interruptRequested_.set();
    searchPromise_->wait();

    searchPromise_ = std::make_unique<QtPromise::QPromise<void>>(
        QtPromise::resolve(
            QtConcurrent::run( this, &QuickFind::doSearchForward, selection, matcher ) )
            .then( [this]( Portion line_found ) {
                emit searchDone( line_found.isValid(), line_found );
            } ) );
}

void QuickFind::searchBackward( Selection selection, QuickFindMatcher matcher )
{
    incrementalSearchStatus_ = IncrementalSearchStatus();
    interruptRequested_.set();
    searchPromise_->wait();

    searchPromise_ = std::make_unique<QtPromise::QPromise<void>>(
        QtPromise::resolve(
            QtConcurrent::run( this, &QuickFind::doSearchBackward, selection, matcher ) )
            .then( [this]( Portion line_found ) {
                emit searchDone( line_found.isValid(), line_found );
            } ) );
}

Portion QuickFind::doSearchForward( const Selection& selection, const QuickFindMatcher& matcher )
{
    return doSearchForward( selection.getNextPosition(), selection, matcher );
}

// Internal implementation of forward search,
// returns the line where the pattern is found or -1 if not found.
// Parameters are the position the search shall start
Portion QuickFind::doSearchForward( const FilePosition& start_position, const Selection& selection,
                                    const QuickFindMatcher& matcher )
{
    interruptRequested_.clear();

    bool found = false;
    int found_start_col{};
    int found_end_col{};

    if ( !matcher.isActive() )
        return {};

    // Optimisation: if we are already after the last match,
    // we don't do any search at all.
    if ( lastMatch_.isLater( start_position ) ) {
        // Send a notification
        sendNotification( QFNotificationReachedEndOfFile() );

        return {};
    }

    auto line = start_position.line();
    LOG( logDEBUG ) << "Start searching at line " << line;
    // We look at the rest of the first line
    if ( matcher.isLineMatching( logData_.getExpandedLineString( line ),
                                 start_position.column() ) ) {
        matcher.getLastMatch( &found_start_col, &found_end_col );
        found = true;
    }
    else {
        searchingNotifier_.reset();
        // And then the rest of the file
        const auto nb_lines = logData_.getNbLine();
        ++line;
        while ( line < nb_lines ) {
            if ( matcher.isLineMatching( logData_.getExpandedLineString( line ) ) ) {
                matcher.getLastMatch( &found_start_col, &found_end_col );
                found = true;
                break;
            }
            ++line;

            // See if we need to notify of the ongoing search
            searchingNotifier_.ping( line.get(), nb_lines.get() );

            if ( interruptRequested_ ) {
                break;
            }
        }
    }

    if ( found ) {
        // Clear any notification
        emit clearNotification();

        return Portion{ line, found_start_col, found_end_col };
    }
    else {
        if ( !interruptRequested_ ) {
            // Update the position of the last match
            FilePosition last_match_position = selection.getPreviousPosition();
            lastMatch_.set( last_match_position );

            // Send a notification
            sendNotification( QFNotificationReachedEndOfFile{} );
        }
        else {
            // Send a notification
            sendNotification( QFNotificationInterrupted{} );
        }

        return {};
    }
}

Portion QuickFind::doSearchBackward( const Selection& selection, const QuickFindMatcher& matcher )
{
    return doSearchBackward( selection.getPreviousPosition(), selection, matcher );
}

// Internal implementation of backward search,
// returns the line where the pattern is found or -1 if not found.
// Parameters are the position the search shall start
Portion QuickFind::doSearchBackward( const FilePosition& start_position, const Selection& selection,
                                     const QuickFindMatcher& matcher )
{
    interruptRequested_.clear();

    bool found = false;
    int start_col{};
    int end_col{};

    if ( !matcher.isActive() )
        return {};

    // Optimisation: if we are already before the first match,
    // we don't do any search at all.
    if ( firstMatch_.isSooner( start_position ) ) {
        // Send a notification
        sendNotification( QFNotificationReachedBegininningOfFile() );

        return {};
    }

    auto line = start_position.line();
    LOG( logDEBUG ) << "Start searching at line " << line;
    // We look at the beginning of the first line
    if ( ( start_position.column() > 0 )
         && ( matcher.isLineMatchingBackward( logData_.getExpandedLineString( line ),
                                              start_position.column() ) ) ) {
        matcher.getLastMatch( &start_col, &end_col );
        found = true;
    }
    else {
        searchingNotifier_.reset();
        // And then the rest of the file
        const auto nb_lines = logData_.getNbLine();
        if ( line.get() > 0 ) {
            --line;
            while ( true ) {
                if ( matcher.isLineMatchingBackward( logData_.getExpandedLineString( line ) ) ) {
                    matcher.getLastMatch( &start_col, &end_col );
                    found = true;
                    break;
                }
                if ( line.get() == 0 ) {
                    break;
                }

                --line;

                // See if we need to notify of the ongoing search
                searchingNotifier_.ping( -static_cast<qint64>( line.get() ), nb_lines.get() );

                if ( interruptRequested_ ) {
                    break;
                }
            }
        }
    }

    if ( found ) {
        // Clear any notification
        emit clearNotification();

        return Portion{ line, start_col, end_col };
    }
    else {
        if ( !interruptRequested_ ) {
            // Update the position of the first match
            FilePosition first_match_position = selection.getNextPosition();
            firstMatch_.set( first_match_position );

            // Send a notification
            sendNotification( QFNotificationReachedBegininningOfFile() );
        }
        else {
            // Send a notification
            sendNotification( QFNotificationInterrupted{} );
        }

        return {};
    }
}

void QuickFind::resetLimits()
{
    lastMatch_.reset();
    firstMatch_.reset();
}

void QuickFind::sendNotification( QFNotification notification )
{
    QMetaObject::invokeMethod(
        this, "notify", Qt::QueuedConnection, Q_ARG(const QFNotification&, notification) );
}
