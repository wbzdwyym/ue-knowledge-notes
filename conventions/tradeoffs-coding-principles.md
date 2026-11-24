# Tradeoffs: Coding Principles

> A conscious bias toward deliberation over speed. For trivial tasks, exercise judgment.

## 1. Think Before You Code

Do not assume. Do not paper over confusion. Surface tradeoffs explicitly.

**Before implementing:**

- State your assumptions in writing. If uncertain, ask.
- If multiple interpretations exist, list them — do not silently pick one.
- If a simpler approach exists, call it out. Object when necessary.
- If something is unclear, stop. Articulate the confusion point and ask.

## 2. Simplicity First

Write the minimum code that solves the problem. No speculative generality.

- Do not add functionality beyond what is requested.
- Do not abstract one-shot code.
- Do not add "flexibility" or "configurability" that is not asked for.
- Do not write error handling for scenarios that cannot occur.
- If you wrote 200 lines but 50 would suffice, rewrite.
- Ask yourself: "Would a senior engineer consider this over-engineered?" If yes, keep simplifying.

## 3. Surgical Changes

Change only what must change. Clean only what you break.

**When editing existing code:**

- Do not "opportunistically" clean up adjacent code, comments, or formatting.
- Do not refactor what is not broken.
- Preserve the existing style, even if you would have written it differently.
- If you notice unrelated dead code, point it out — but do not delete it.

**When your changes create residue:**

- Remove imports, variables, or functions that your changes made unused.
- Do not remove pre-existing dead code unless explicitly asked.

**Test:** every changed line should be directly traceable to the user's request.

## 4. Goal-Driven Execution

Define the success criterion first, then iterate until it passes.

**Convert tasks into verifiable objectives:**

| Vague request | Verifiable goal |
|---|---|
| "Add validation" | "Write tests for invalid input first, then make them pass" |
| "Fix this bug" | "Write a test that reproduces it first, then make it pass" |
| "Refactor X" | "Tests pass identically before and after the change" |

**For multi-step tasks, give a short plan first:**

1. [Step] → Verify: [check]
2. [Step] → Verify: [check]
3. [Step] → Verify: [check]

A strong success criterion lets you close the loop independently. A weak one ("make it work") requires repeated clarification.
