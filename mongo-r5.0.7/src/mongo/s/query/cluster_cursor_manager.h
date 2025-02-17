/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/cursor_id.h"
#include "mongo/db/generic_cursor.h"
#include "mongo/db/kill_sessions.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/session_killer.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/s/query/cluster_client_cursor.h"
#include "mongo/s/query/cluster_client_cursor_guard.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClockSource;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * ClusterCursorManager is a container for ClusterClientCursor objects.  It manages the lifetime of
 * its registered cursors and tracks basic information about them.
 *
 * Each registered cursor is either in a 'pinned' or an 'idle' state.  Registered cursors must be
 * pinned in order to iterate results, and cursors may only be pinned by one client at a time (this
 * ensures that the result stream is only directed to a single client at a time).  Pinning a cursor
 * transfers ownership of the cursor to a PinnedCursor object (although the manager maintains
 * information about registered cursors even when they're pinned).  Ownership is transferred back to
 * the manager by calling PinnedCursor::returnCursor().
 *
 * The manager supports killing of registered cursors, either through the PinnedCursor object or
 * with the kill*() suite of methods.
 *
 * No public methods throw exceptions, and all public methods are thread-safe.
 */ //Grid._cursorManagerȫ�ֱ���
class ClusterCursorManager {
    ClusterCursorManager(const ClusterCursorManager&) = delete;
    ClusterCursorManager& operator=(const ClusterCursorManager&) = delete;

public:
    //
    // Enum/struct declarations, for use with public methods below.
    //

    enum class CursorType {
        // Represents a cursor retrieving data from a single remote source.
        SingleTarget,

        // Represents a cursor retrieving data from multiple remote sources.
        MultiTarget,
    };

    enum class CursorLifetime {
        // Represents a cursor that should be killed automatically after a period of inactivity.
        Mortal,

        // Represents a "no timeout" cursor.
        Immortal,
    };

    enum class CursorState {
        // Represents a non-exhausted cursor.
        NotExhausted,

        // Represents an exhausted cursor.
        Exhausted,
    };

    struct Stats {
        // Count of open cursors registered with CursorType::MultiTarget.
        size_t cursorsMultiTarget = 0;

        // Count of open cursors registered with CursorType::SingleTarget.
        size_t cursorsSingleTarget = 0;

        // Count of pinned cursors.
        size_t cursorsPinned = 0;
    };

    // Represents a function that may be passed into a ClusterCursorManager method which checks
    // whether the current client is authorized to perform the operation in question. The function
    // will be passed the list of users authorized to use the cursor.
    using AuthzCheckFn = std::function<Status(UserNameIterator)>;

    /**
     * PinnedCursor is a moveable, non-copyable class representing ownership of a cursor that has
     * been leased from a ClusterCursorManager.
     *
     * A PinnedCursor can either be in a state where it owns a cursor, or can be in a null state
     * where it owns no cursor.  If a cursor is owned, the underlying cursor can be iterated with
     * next(), and the underlying cursor can be returned to the manager with the returnCursor()
     * method (and after it is returned, no cursor will be owned). When a PinnedCursor is created,
     * the underlying cursor is attached to the current OperationContext.
     *
     * Invoking the PinnedCursor's destructor while it owns a cursor will kill, detach from the
     * current OperationContext, and return the cursor.
     */
    class PinnedCursor {
        PinnedCursor(const PinnedCursor&) = delete;
        PinnedCursor& operator=(const PinnedCursor&) = delete;

    public:
        /**
         * Creates a PinnedCursor owning no cursor.
         */
        PinnedCursor() = default;

        /**
         * If a cursor is not owned, performs no action.  Otherwise, informs the manager that the
         * cursor should be killed, and transfers ownership of the cursor back to the manager.
         */
        ~PinnedCursor();

        /**
         * Creates a PinnedCursor by moving from 'other'.
         */
        PinnedCursor(PinnedCursor&& other);

        /**
         * Moves 'other' into '*this'.  If '*this' was owning a cursor, informs the manager that the
         * cursor should be killed, and transfers ownership of the cursor back to the manager.
         */
        PinnedCursor& operator=(PinnedCursor&& other);

        /**
         * Returns a pointer to the ClusterClientCursor that this PinnedCursor owns. A cursor must
         * be owned.
         */
        ClusterClientCursor* operator->() const {
            invariant(_cursor);
            return _cursor.get();
        }

        /**
         * Transfers ownership of the underlying cursor back to the manager, and detaches it from
         * the current OperationContext. A cursor must be owned, and a cursor will no longer be
         * owned after this method completes.
         *
         * If 'Exhausted' is passed, the manager will de-register and destroy the cursor after it
         * is returned.
         */
        void returnCursor(CursorState cursorState);

        /**
         * Returns the cursor id for the underlying cursor, or zero if no cursor is owned.
         */
        CursorId getCursorId() const;

        /**
         * Returns a GenericCursor version of the pinned cursor.
         */
        GenericCursor toGenericCursor() const;

    private:
        // ClusterCursorManager is a friend so that its methods can call the PinnedCursor
        // constructor declared below, which is private to prevent clients from calling it directly.
        friend class ClusterCursorManager;

        /**
         * Creates a PinnedCursor owning the given cursor, which must be checked out from the given
         * manager.  Does not take ownership of 'manager'.  'manager' and 'cursor' must be non-null,
         * and 'cursorId' must be non-zero.
         */
        PinnedCursor(ClusterCursorManager* manager,
                     ClusterClientCursorGuard&& cursor,
                     const NamespaceString& nss,
                     CursorId cursorId);

        /**
         * Informs the manager that the cursor should be killed, and transfers ownership of the
         * cursor back to the manager.  A cursor must be owned.
         */
        void returnAndKillCursor();

        ClusterCursorManager* _manager = nullptr;
        std::unique_ptr<ClusterClientCursor> _cursor;
        NamespaceString _nss;
        CursorId _cursorId = 0;
    };

    /**
     * CursorEntry is a movable, non-copyable container for a single cursor.
     */
    class CursorEntry {
    public:
        CursorEntry() = default;

        CursorEntry(std::unique_ptr<ClusterClientCursor> cursor,
                    CursorType cursorType,
                    CursorLifetime cursorLifetime,
                    Date_t lastActive,
                    UserNameIterator authenticatedUsersIter,
                    UUID clientUUID,
                    boost::optional<OperationKey> opKey)
            : _cursor(std::move(cursor)),
              _cursorType(cursorType),
              _cursorLifetime(cursorLifetime),
              _lastActive(lastActive),
              _lsid(_cursor->getLsid()),
              _opKey(std::move(opKey)),
              _originatingClient(std::move(clientUUID)),
              _authenticatedUsers(
                  userNameIteratorToContainer<std::vector<UserName>>(authenticatedUsersIter)) {
            invariant(_cursor);
        }

        CursorEntry(const CursorEntry&) = delete;
        CursorEntry& operator=(const CursorEntry&) = delete;

        CursorEntry(CursorEntry&& other) = default;
        CursorEntry& operator=(CursorEntry&& other) = default;

        bool isKillPending() const {
            // A cursor is kill pending if it's checked out by an OperationContext that was
            // interrupted.
            if (!_operationUsingCursor) {
                return false;
            }

            // Must hold the Client lock when calling isKillPending().
            stdx::unique_lock<Client> lk(*_operationUsingCursor->getClient());
            return _operationUsingCursor->isKillPending();
        }

        CursorType getCursorType() const {
            return _cursorType;
        }

        CursorLifetime getLifetimeType() const {
            return _cursorLifetime;
        }

        Date_t getLastActive() const {
            return _lastActive;
        }

        boost::optional<LogicalSessionId> getLsid() const {
            return _lsid;
        }

        boost::optional<OperationKey> getOperationKey() const {
            return _opKey;
        }

        /**
         * Returns a cursor guard holding the cursor owned by this CursorEntry for an operation to
         * use. Only one operation may use the cursor at a time, so callers should check that
         * getOperationUsingCursor() returns null before using this function. Callers may not pass
         * nullptr for opCtx. Ownership of the cursor is given to the returned
         * ClusterClientCursorGuard; callers that want to assume ownership over the cursor directly
         * must unpack the cursor from the returned guard.
         */
        ClusterClientCursorGuard releaseCursor(OperationContext* opCtx) {
            invariant(!_operationUsingCursor);
            invariant(_cursor);
            invariant(opCtx);
            _operationUsingCursor = opCtx;
            return ClusterClientCursorGuard(opCtx, std::move(_cursor));
        }

        /**
         * Creates a generic cursor from the cursor inside this entry. Should only be called on
         * idle cursors. The caller must supply the cursorId and namespace because the CursorEntry
         * does not have access to them.  Cannot be called if this CursorEntry does not own an
         * underlying ClusterClientCursor.
         */
        GenericCursor cursorToGenericCursor(CursorId cursorId, const NamespaceString& ns) const;

        OperationContext* getOperationUsingCursor() const {
            return _operationUsingCursor;
        }

        /**
         * Indicate that the cursor is no longer in use by an operation. Once this is called,
         * another operation may check the cursor out.
         */
        void returnCursor(std::unique_ptr<ClusterClientCursor> cursor) {
            invariant(cursor);
            invariant(!_cursor);
            invariant(_operationUsingCursor);

            _cursor = std::move(cursor);
            _operationUsingCursor = nullptr;
        }

        void setLastActive(Date_t lastActive) {
            _lastActive = lastActive;
        }

        UserNameIterator getAuthenticatedUsers() const {
            return makeUserNameIterator(_authenticatedUsers.begin(), _authenticatedUsers.end());
        }

        const UUID& originatingClientUuid() const {
            return _originatingClient;
        }

    private:
        std::unique_ptr<ClusterClientCursor> _cursor;
        CursorType _cursorType = CursorType::SingleTarget;
        CursorLifetime _cursorLifetime = CursorLifetime::Mortal;
        Date_t _lastActive;
        boost::optional<LogicalSessionId> _lsid;

        /**
         * The client OperationKey from the OperationContext at the time of registering a cursor.
         */
        boost::optional<OperationKey> _opKey;

        /**
         * Current operation using the cursor. Non-null if the cursor is checked out.
         */
        OperationContext* _operationUsingCursor = nullptr;

        /**
         * The UUID of the Client that opened the cursor.
         */
        UUID _originatingClient;

        /**
         * The set of users authorized to use this cursor.
         */
        std::vector<UserName> _authenticatedUsers;
    };

    /**
     * Constructs an empty manager.
     *
     * Does not take ownership of 'clockSource'.  'clockSource' must refer to a non-null clock
     * source that is valid for the lifetime of the constructed ClusterCursorManager.
     */
    explicit ClusterCursorManager(ClockSource* clockSource);

    /**
     * Can only be called if the manager no longer owns any cursors.
     */
    ~ClusterCursorManager();

    /**
     * Kills and reaps all cursors currently owned by this cursor manager, and puts the manager
     * into the shutting down state where it will not accept any new cursors for registration.
     */
    void shutdown(OperationContext* opCtx);

    /**
     * Registers the given cursor with this manager, and returns the registered cursor's id, or
     * a non-OK status if something went wrong.
     *
     * 'cursor' must be non-null.  'cursorType' should reflect whether or not the cursor is
     * operating on a sharded namespace (this will be used for reporting purposes).
     * 'cursorLifetime' should reflect whether or not this cursor should be immune from the idle
     * cursor destruction procedure.
     *
     * If the OperationContext has a deadline set (from a maxTimeMS), stashes the remaining time
     * limit on 'cursor' for use in subsequent getMores.
     *
     * On an error return, kills 'cursor'.
     *
     * Does not block.
     */
    StatusWith<CursorId> registerCursor(OperationContext* opCtx,
                                        std::unique_ptr<ClusterClientCursor> cursor,
                                        const NamespaceString& nss,
                                        CursorType cursorType,
                                        CursorLifetime cursorLifetime,
                                        UserNameIterator authenticatedUsers);

    /**
     * Moves the given cursor to the 'pinned' state, and transfers ownership of the cursor to the
     * PinnedCursor object returned.  Cursors that are pinned must later be returned with
     * PinnedCursor::returnCursor().
     *
     * Only one client may pin a given cursor at a time.  If the given cursor is already pinned,
     * returns an error Status with code CursorInUse.  If the given cursor is not registered or has
     * a pending kill, returns an error Status with code CursorNotFound.
     *
     * Checking out a cursor will attach it to the given operation context.
     *
     * 'authChecker' is function that will be called with the list of users authorized to use this
     * cursor. This function should check whether the current client is also authorized to use this
     * cursor, and if not, return an error status, which will cause checkOutCursor to fail.
     *
     * If 'checkSessionAuth' is 'kCheckSession' or left unspecified, this function also checks if
     * the current session in the specified 'opCtx' has privilege to access the cursor specified by
     * 'id.' In this case, this function returns a 'mongo::Status' with information regarding the
     * nature of the inaccessability when the cursor is not accessible. If 'kNoCheckSession' is
     * passed for 'checkSessionAuth,' this function does not check if the current session is
     * authorized to access the cursor with the given id.
     *
     * This method updates the 'last active' time associated with the cursor to the current time.
     *
     * Does not block.
     */
    enum AuthCheck { kCheckSession = true, kNoCheckSession = false };
    StatusWith<PinnedCursor> checkOutCursor(const NamespaceString& nss,
                                            CursorId cursorId,
                                            OperationContext* opCtx,
                                            AuthzCheckFn authChecker,
                                            AuthCheck checkSessionAuth = kCheckSession);

    /**
     * This method will find the given cursor, and if it exists, call 'authChecker', passing the
     * list of users authorized to use the cursor. Will propagate the return value of authChecker.
     */
    Status checkAuthForKillCursors(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   CursorId cursorId,
                                   AuthzCheckFn authChecker);


    /**
     * Informs the manager that the given cursor should be killed.  The cursor need not necessarily
     * be in the 'idle' state, and the lifetime type of the cursor is ignored.
     *
     * If the given cursor is not registered, returns an error Status with code CursorNotFound.
     * Otherwise, marks the cursor as 'kill pending' and returns Status::OK().
     *
     * A thread which is currently using a cursor may not call killCursor() on it, but rather
     * should kill the cursor by checking it back into the manager in the exhausted state.
     *
     * May block waiting for other threads to finish, but does not block on the network.
     */
    Status killCursor(OperationContext* opCtx, const NamespaceString& nss, CursorId cursorId);

    /**
     * Kill the cursors satisfying the given predicate. Returns the number of cursors killed.
     */
    std::size_t killCursorsSatisfying(
        OperationContext* opCtx, const std::function<bool(CursorId, const CursorEntry&)>& pred);

    /**
     * Informs the manager that all mortal cursors with a 'last active' time equal to or earlier
     * than 'cutoff' should be killed.  The cursors need not necessarily be in the 'idle' state.
     *
     * May block waiting for other threads to finish, but does not block on the network.
     *
     * Returns the number of cursors that were killed due to inactivity.
     */
    std::size_t killMortalCursorsInactiveSince(OperationContext* opCtx, Date_t cutoff);

    /**
     * Kills all cursors which are registered at the time of the call. If a cursor is registered
     * while this function is running, it may not be killed. If the caller wants to guarantee that
     * all cursors are killed, shutdown() should be used instead.
     *
     * May block waiting for other threads to finish, but does not block on the network.
     */
    void killAllCursors(OperationContext* opCtx);

    /**
     * Returns the number of open cursors on a ClusterCursorManager, broken down by type.
     *
     * Does not block.
     */
    Stats stats() const;

    /**
     * Appends sessions that have open cursors in this cursor manager to the given set of lsids.
     */
    void appendActiveSessions(LogicalSessionIdSet* lsids) const;

    /**
     * Returns a list of GenericCursors for all idle (non-pinned) cursors in the cursor manager.
     */
    std::vector<GenericCursor> getIdleCursors(
        const OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const;

    std::pair<Status, int> killCursorsWithMatchingSessions(OperationContext* opCtx,
                                                           const SessionKiller::Matcher& matcher);

    /**
     * Returns a list of all open cursors for the given session.
     */
    stdx::unordered_set<CursorId> getCursorsForSession(LogicalSessionId lsid) const;

    /*
     * Returns a list of all open cursors for the given set of OperationKeys.
     */
    stdx::unordered_set<CursorId> getCursorsForOpKeys(std::vector<OperationKey>) const;

    /**
     * Returns the namespace associated with the given cursor id, by examining the 'namespace
     * prefix' portion of the cursor id.  A cursor with the given cursor id need not actually exist.
     * If no such namespace is associated with the 'namespace prefix' portion of the cursor id,
     * returns boost::none.
     *
     * This method is deprecated.  Use only when a cursor needs to be operated on in cases where a
     * namespace is not available (e.g. OP_KILL_CURSORS).
     *
     * Does not block.
     */
    boost::optional<NamespaceString> getNamespaceForCursorId(CursorId cursorId) const;

    void incrementCursorsTimedOut(size_t inc) {
        _cursorsTimedOut += inc;
    }

    size_t cursorsTimedOut() const {
        return _cursorsTimedOut;
    }

private:
    struct CursorEntryContainer;
    using CursorEntryMap = stdx::unordered_map<CursorId, CursorEntry>;
    using NssToCursorContainerMap = stdx::unordered_map<NamespaceString, CursorEntryContainer>;

    // Internal, fixed size log of events cursor manager. This has been added to help diagnose
    // SERVER-27796.
    struct LogEvent {
        enum class Type {
            kRegisterAttempt,   // Any attempt to create a cursor.
            kRegisterComplete,  // A cursor actually being created.

            // Checking out a cursor.
            kCheckoutAttempt,
            kCheckoutComplete,

            // Caller attempts to check a cursor in. This event may be followed by a
            // kCheckInCompleteCursorSaved, or by events which indicate the cursor is deleted.
            kCheckInAttempt,

            // Logged when the check-in is successful and the cursor is kept.
            kCheckInCompleteCursorSaved,

            // Detaching a cursor (and erasing associated namespace).
            kDetachAttempt,
            kDetachComplete,
            kNamespaceEntryMapErased,

            // These mark the beginning and end of the period where
            // killCursorsSatisfyingPredicate() holds a lock.
            kRemoveCursorsSatisfyingPredicateAttempt,
            kRemoveCursorsSatisfyingPredicateComplete,

            // Any call to killCursor().
            kKillCursorAttempt,

            // Represents each time killCursorsSatisfyingPredicate() detaches a cursor that it
            // intends to destroy.
            kCursorMarkedForDeletionBySatisfyingPredicate,

            //
            // NOTE: If you ever add to this enum be sure to update the typeToString() method
            // below.
            //
        };

        static std::string typeToString(Type);

        Type type;

        // boost::none for log entries that don't have an associated cursor ID.
        boost::optional<CursorId> cursorId;

        // Time is not always provided to avoid having to read the clock while the mutex is held.
        boost::optional<Date_t> time;
        boost::optional<NamespaceString> nss;
    };

    // Circular queue used to store the latest events that happened in the ClusterCursorManager.
    struct CircularLogQueue {
        std::vector<LogEvent> events{512};
        size_t start = 0;
        size_t end = 0;

        void push(LogEvent&& e) {
            events[end] = std::move(e);
            end = (end + 1) % events.size();
            if (end == start) {
                start = (start + 1) % events.size();
            }
        }
    };

    /**
     * Transfers ownership of the given pinned cursor back to the manager, and moves the cursor to
     * the 'idle' state.
     *
     * If 'cursorState' is 'Exhausted', the cursor will be destroyed.
     *
     * Thread-safe.
     *
     * Intentionally private.  Clients should use public methods on PinnedCursor to check a cursor
     * back in.
     */
    void checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                       const NamespaceString& nss,
                       CursorId cursorId,
                       CursorState cursorState);

    /**
     * Will detach a cursor, release the lock and then call kill() on it.
     */
    void detachAndKillCursor(stdx::unique_lock<Latch> lk,
                             OperationContext* opCtx,
                             const NamespaceString& nss,
                             CursorId cursorId);

    /**
     * Returns a pointer to the CursorEntry for the given cursor.  If the given cursor is not
     * registered, returns null.
     *
     * Not thread-safe.
     */
    CursorEntry* _getEntry(WithLock, NamespaceString const& nss, CursorId cursorId);

    /**
     * De-registers the given cursor, and returns an owned pointer to the underlying
     * ClusterClientCursor object.
     *
     * If the given cursor is pinned, returns an error Status with code CursorInUse.  If the given
     * cursor is not registered, returns an error Status with code CursorNotFound.
     *
     * Not thread-safe.
     */
    StatusWith<ClusterClientCursorGuard> _detachCursor(WithLock,
                                                       OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       CursorId cursorId);

    /**
     * Flags the OperationContext that's using the given cursor as interrupted.
     */
    void killOperationUsingCursor(WithLock, CursorEntry* entry);

    /**
     * CursorEntryContainer is a moveable, non-copyable container for a set of cursors, where all
     * contained cursors share the same 32-bit prefix of their cursor id.
     */
    struct CursorEntryContainer {
        CursorEntryContainer(const CursorEntryContainer&) = delete;
        CursorEntryContainer& operator=(const CursorEntryContainer&) = delete;

        CursorEntryContainer(uint32_t containerPrefix) : containerPrefix(containerPrefix) {}

        CursorEntryContainer(CursorEntryContainer&& other) = default;
        CursorEntryContainer& operator=(CursorEntryContainer&& other) = default;

        // Common cursor id prefix for all cursors in this container.
        uint32_t containerPrefix;

        // Map from cursor id to cursor entry.
        CursorEntryMap entryMap;
    };

    /**
     * Erase the container that 'it' points to and return an iterator to the next one. Assumes 'it'
     * is an iterator in '_namespaceToContainerMap'.
     */
    NssToCursorContainerMap::iterator eraseContainer(NssToCursorContainerMap::iterator it);

    /**
     * Functions which dump the state/history of the cursor manager into a BSONObj for debug
     * purposes.
     */
    BSONObj dumpCursorIdToNssMap() const;
    BSONObj dumpNssToContainerMap() const;
    BSONObj dumpInternalLog() const;

    /**
     * Logs objects which summarize the current state of the cursor manager as well as its recent
     * history.
     */
    void logCursorManagerInfo() const;

    // Clock source.  Used when the 'last active' time for a cursor needs to be set/updated.  May be
    // concurrently accessed by multiple threads.
    ClockSource* _clockSource;

    // Synchronizes access to all private state variables below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ClusterCursorManager::_mutex");

    bool _inShutdown{false};

    // Randomness source.  Used for cursor id generation.
    const int64_t _randomSeed;
    PseudoRandom _pseudoRandom;

    // Map from cursor id prefix to associated namespace.  Exists only to provide namespace lookup
    // for (deprecated) getNamespaceForCursorId() method.
    //
    // A CursorId is a 64-bit type, made up of a 32-bit prefix and a 32-bit suffix.  When the first
    // cursor on a given namespace is registered, it is given a CursorId with a prefix that is
    // unique to that namespace, and an arbitrary suffix.  Cursors subsequently registered on that
    // namespace will all share the same prefix.
    //
    // Entries are added when the first cursor on the given namespace is registered, and removed
    // when the last cursor on the given namespace is destroyed.
    stdx::unordered_map<uint32_t, NamespaceString> _cursorIdPrefixToNamespaceMap;

    // Map from namespace to the CursorEntryContainer for that namespace.
    //
    // Entries are added when the first cursor on the given namespace is registered, and removed
    // when the last cursor on the given namespace is destroyed.
    NssToCursorContainerMap _namespaceToContainerMap;

    size_t _cursorsTimedOut = 0;

    CircularLogQueue _log;
};

}  // namespace mongo
