# Contract-based isolation and AI agency

Status: ARCHITECTURE, agreed 2026-08-05. Nothing below is implemented yet.
Tracking: #680 (architecture), #681 (consent/revocation/revert), #682 (device
identity), plus the component tickets listed at the end.

## 1. Purpose

The end goal is to let the AI chat and AI agents interact with apps, APIs,
services, devices and the filesystem in a way that is **auditable**, **can be
rolled back**, and is **controlled by an abstracted layer**.

App-to-app isolation is the substrate that makes that possible. It is not the
goal in itself. Every design decision here should be read against the question
"does this make AI agency bounded, visible and reversible?".

## 2. The inversion

POSIX is *permitted unless a mode bit denies*. This model is *denied unless a
contract permits*.

The uid/mode work (#674, #679) is therefore **not** the security model. It is:

1. the substrate that stops the contract layer being bypassed, and
2. a backstop for anything the contract layer does not yet cover.

A system where every process runs as uid 0 cannot enforce contracts, because the
permission check short-circuits before it looks at anything. That is why #679 is
a prerequisite for all of this, and why it is a prerequisite under every variant
of the design.

## 3. Identity

Identities are plural and heterogeneous. All of the following are identities:

- **app binaries**
- **services**
- **users**
- **devices**
- **AI tasks** (see below)

**An app may hold several identities**, each with its own contracts. The same
binary acting as "the indexer" and as "the user's editor" is two principals with
different authority. This is deliberate: it grants a component narrow authority
without forcing it to be split into separate binaries.

### AI identity is per task. Non-negotiable.

Each AI task or conversation gets its **own** identity. There is no standing "AI
Chat" principal. Contracts are issued to the task identity and expire with it.

This is the keystone, because the per-task identity is simultaneously:

```
identity boundary == contract scope == audit unit == rollback/transaction unit
```

"Revert this task" is a coherent operation precisely because the identity is per
task. "Revert what the AI did" never could be. A task is a transaction.

## 4. Contracts

Isolation is the default. Interaction happens only through a contract that is:

- **explicit** - made, never inherited or assumed
- **time-limited** - it expires
- **scope/context-limited** - a named capability, not an ambient right
- **directional** - A may call B without B gaining any call into A

Two further properties follow necessarily and must be designed in rather than
bolted on:

- **revocable**
- **auditable** - a contract nobody can see or withdraw is not a contract

## 5. Escrow

An app does **not** negotiate with its target. It requests a contract from the
target **through an escrow**: the OS-held default contract definitions and
security profile.

Consequences, all of them desirable:

- policy lives in one auditable place
- targets stay dumb; they honour a contract, they do not implement policy
- defaults and consent are expressed once, centrally

**The escrow is the crown jewels.** Anything that can talk it into issuing a
contract owns the machine. It must be unreachable by the AI, not merely
off-limits by convention. This is the immutable security core (#305). It cannot
be an ordinary userland service that AI-authored code could patch.

## 6. Enforcement

Enforcement is at the **syscall chokepoint**. Checked anywhere higher, for
example in libc, an app simply does not call libc.

Revocation must bite **in flight**, not only on the next request. A long-running
operation holding a contract must be interrupted, or fail its next check. A
revocation that takes effect eventually is not a revocation.

## 7. Rollback, and its honest boundary

Auditable needs a journal. Rollback needs effects to be **undoable**. Effects
divide three ways, and the contract definition must declare which **before**
execution:

| class | examples | can it be undone |
|---|---|---|
| **reversible** | filesystem writes, settings, app state | yes, genuinely, via journal plus snapshot |
| **compensatable** | turn the light back off, cancel the print | by inverse action only; the intermediate state may have been observed |
| **irreversible** | a POST that reached a remote server, a sent message, money moved, filament consumed | no |

Irreversible effects cannot be made safe by rollback. They are made safe
**before** the fact: tighter scope, shorter time, and explicit user consent at
contract-request time. Irreversibility is the primary axis of "risky".

Treating all effects as uniformly rollbackable would produce something that feels
safe and is not.

### Binding rule: the undo surface must never lie

If a task performed nine reversible actions and one irreversible one, "revert
task" reverts the nine and states plainly, naming it, what it could not take
back. A system that claims complete undo and quietly cannot deliver is worse than
one that is honest about its edge, because the user stops checking.

On failure, a task auto-reverts its reversible effects rather than leaving the
system half-changed.

## 8. Consent and revocation

- **New or risky** contract requests are presented to the user before taking
  effect. Risky means at least: can produce irreversible effects (primary),
  touches secrets, touches devices, permits network egress, unusually broad
  scope, unusually long duration.
- The user can **revoke any contract at any time**, from a contract manager that
  lists holder identity, target, scope, expiry and direction.
- Routine, narrow, reversible, previously-approved requests are handled silently
  by escrow defaults.

**Prompt fatigue is the failure mode.** This is the UAC lesson: prompt on
everything and users click through everything, leaving friction and no security.
The quiet path staying quiet is a feature to be tested, not an accident.

## 9. Device identity

Devices lie about identity. We have already been bitten by USB passthrough
reporting a bogus identity, so this is not hypothetical.

The user validates devices: trust on first use, the SSH host-key pattern, reusing
the existing `/CONFIG/KNOWN_HOSTS` flow rather than inventing a parallel one.

- The approval prompt must **state its own confidence**. "This device reports a
  unique serial" and "this device cannot be distinguished from any other of its
  model" are very different approvals. VID/PID alone is spoofable and not unique.
- **Re-validate on capability change, not only identity change.** The attack that
  matters is BadUSB-shaped: the approved audio device later adds a keyboard
  interface. Identity unchanged, authority wildly changed. A device's approved
  capability set is part of what was approved.
- Topology is recorded and shown as **context**, not folded into identity, so
  moving a device between ports is informational rather than alarming.

Validating a device *is* authorising a contract for that identity, so it uses the
same escrow, consent surface and manager as everything else.

## 10. POSIX compatibility

POSIX-shaped access is expressible **as** a contract, including contracts of
unlimited time or unlimited scope. Those exist deliberately and are discouraged.

Where a broad contract looks necessary, first try to **remove the need**. Example:
libc reading `/CONFIG/PASSWD` and `GROUP` for uid-to-name lookup is better served
by a name-resolution service the app calls, so the contract is with a service and
carries no file access at all.

### Anti-pattern to design against

The unlimited-time/unlimited-scope hatch will silently become the default unless
it is made **visibly expensive**. Enumerate every unlimited contract, audit them,
and put the count somewhere it is seen. Otherwise every contract quietly becomes
unlimited and we have reimplemented POSIX with extra steps. Make the wrong thing
hard, not merely discouraged. This is the same lesson as the build-gate drift:
a rule that relies on remembering is not a control.

## 11. Threat model

What this defends against:

- an app reading another app's data without a contract
- a compromised or malicious app exceeding its granted authority
- an AI agent, including one steered by prompt injection, acting outside the
  scope and time it was granted
- a substituted device impersonating an approved one

What it does **not** defend against, and must not be described as if it does:

- an approved device that was malicious from the start (mitigated by capability
  scope and re-validation, not eliminated)
- effects that already left the machine (see 7)
- compromise of the escrow itself, which is why the escrow is the immutable core

**Prompt injection becomes a privilege-escalation vector** once an agent holds
contracts. Nova (#449) helps at the input boundary and will never be complete.
The real containment is this architecture: bounded scope plus bounded time plus a
journal converts "the AI was tricked" from unbounded compromise into a bounded,
visible, reversible incident. Design for containment, not for making injection
impossible.

## 12. What "must be 1000% secure" translates to

The token and contract mechanism is the highest-assurance component in the
system. That intent is right, and it needs to become properties that can be
verified rather than a slogan. Nothing is unconditionally secure; the goal is
that any failure requires breaking something we can **name**.

Concretely, for the token and contract layer:

1. **A written threat model** naming the attacker and their capabilities, so
   "secure" has a referent.
2. **No hand-rolled cryptography.** Use a reviewed construction. This codebase
   has already been bitten by a signature that was parsed and never verified
   (#510); parsing is not verifying.
3. **Unforgeability**, bound to identity, scope, expiry and direction, such that
   holding one token grants nothing beyond that tuple.
4. **Revocation that actually revokes**, including in flight. Pure crypto tokens
   are hard to revoke, which forces either short expiry with renewal, or an
   online check. Decide explicitly and record why.
5. **Key storage** that is not a world-readable file. Note the current state:
   `perms_check` had no parent traversal and the desktop runs as root, so today
   any process can read anything. Both are being fixed first for exactly this
   reason.
6. **Tamper-evident storage** for contracts and the audit journal: append-only,
   with detection of truncation and reordering, not only of edits.
7. **Fail closed.** An unreadable, corrupt, expired or unverifiable contract
   denies. A gate that cannot read its input must never report success. This
   codebase has shipped exactly that bug (#622) and must not repeat it.
8. **Adversarial testing that is proven to go red**, not merely green: forged
   token, expired token, wrong-scope token, wrong-direction call, replayed
   token, revoked-mid-operation token. Each must be demonstrated failing before
   the mechanism is trusted.
9. **No bypass path.** Enumerate every route to a protected resource and show
   each passes the chokepoint. The recurring failure in this codebase is a
   second, forgotten path (two `/PANIC.TXT` writers disagreeing, two
   `build-golden.sh` copies, two signer copies).

## 13. Build order

1. **#679** - non-root session plus the no-entry-default write fix. Prerequisite
   for everything: contracts cannot be enforced where every process is uid 0.
   Must not bake in the assumption that all apps share one uid.
2. **Identity registry** and **contract store**, since everything else reads them.
3. **Escrow** and the **token mechanism**, inside the immutable core.
4. **Enforcement** at the syscall chokepoint.
5. **Journal and rollback engine**.
6. **Contract manager** and the consent surface.
7. Migrate existing consumers, starting with the AI chat path, which is the
   reason for the whole exercise.

## 14. Open decisions

- Whether app identity is per-app uid (reusing the uid machinery) or by signed
  binary with uid largely irrelevant. Both work with the above; this decides
  whether the #679 uid work is load-bearing long term or transitional.
- Who may create a contract definition, as distinct from requesting one. If an
  app can grant itself a contract it is theatre.
- Token format and the revocation strategy that follows from it (see 12.4).
