-- dump(obj, depth): object subtree as text (name class flags mesh)
function dumptree(o, maxdepth)
    local out = {}
    local function rec(g, d)
        local x, y, z = g:pos()
        out[#out + 1] = string.format("%s%s [%s] fl=%08X mesh=%08X pos=%.0f,%.0f,%.0f", string.rep("  ", d), g:name(),
            g:class(), g:flags(), g:mesh(), x, y, z)
        if d < (maxdepth or 4) then
            for _, c in ipairs(g:children()) do rec(c, d + 1) end
        end
    end
    rec(o, 0)
    return table.concat(out, "\n")
end
