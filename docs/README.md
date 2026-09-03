# Docs

You can find documentation and other useful resources in this folder.

- [MiSTer Performance Memory](agent-memory/mister-performance.md)
- [MiSTer Ralph Loop V2](agent-memory/mister-ralph-loop-v2.md)
- [MiSTer Ralph Working Brief](agent-memory/mister-ralph-working-brief.md)
- [MiSTer Ralph Working Brief Template](agent-memory/mister-ralph-working-brief-template.md)
- [MiSTer Port Feasibility And Execution Plan](archive/mister-port-plan.md)
- [MiSTer Runbook](mister-runbook.md)
- [MiSTer Wrapper Core](mister-wrapper.md)
- [Training-mode SELECT reset](training-select-reset.md)
- [On-screen text and the 384 px canvas](ui-text-width.md)

## `archive/`

Documents whose work is finished or was dropped: plans that shipped, research
whose conclusions have landed in the code, designs something else replaced.
Each carries a header naming the commit it was true at.

**Go there for *why*** — the decision that was taken and what it was taken
instead of, and above all the negative results: the approach that was tried and
measured and did not work. The code has nowhere to record a thing that isn't
there, so the archive is the only place that knowledge exists, and re-deriving
it costs hours.

**Never go there for what the code does now.** Read the code for that. An
archived document is not maintained and its line numbers describe the tree it
was written against; that is the point of keeping it, not a defect in it.
Nothing under `archive/` is checked by the citation linter, and repointing a
citation inside it is not work.