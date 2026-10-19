#pragma once

// The player's requests to play or add, carried out as if at once though their files may come
// later (a folder listed on a thread): a later request to play supersedes an earlier one still
// being listed; an add goes to the tab shown when it was asked for, and not into a queue played
// over by a request made after it; adds asked for after a play still being listed wait for it.
// Pure: unit-tested.
struct OpenOrder {
    unsigned gen = 0;               // requests to play made
    bool pending = false;           // the latest of them is still being listed

    // a request made now: its number (a play: its own; an add: the play before it)
    unsigned request(bool enqueue) {
        if (!enqueue) {
            ++gen;
            pending = true;
        }
        return gen;
    }

    enum class Then { Do, Drop, Wait };
    // a request's files known (`g`: its number). `tab`: the tab it was asked for in is known
    // (else there was no player); then `tabThere`, and `tabPlayed`: the request that last played
    // into it. A play done (or dropped as its own) is no longer pending: what waits, may go.
    Then ready(bool enqueue, unsigned g, bool tab, bool tabThere, unsigned tabPlayed) {
        if (!enqueue) {
            if (g != gen) return Then::Drop;                // a later one plays instead
            pending = false;
            return Then::Do;
        }
        if (pending && g == gen) return Then::Wait;         // after a play still being listed
        if (tab) return tabThere && tabPlayed <= g ? Then::Do : Then::Drop;
        return g == gen ? Then::Do : Then::Drop;            // (no player then: one played since)
    }

    // a request to play that comes to nothing (no files): what waits for it may go
    void gone(unsigned g) { if (g == gen) pending = false; }
};
