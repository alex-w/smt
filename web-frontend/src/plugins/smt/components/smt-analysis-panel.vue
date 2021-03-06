// Stellarium Web - Copyright (c) 2020 - Stellarium Labs SAS
//
// This program is licensed under the terms of the GNU AGPL v3, or
// alternatively under a commercial licence.
//
// The terms of the AGPL v3 license can be found in the main directory of this
// repository.
//
// This file is part of the Survey Monitoring Tool plugin, which received
// funding from the Centre national d'études spatiales (CNES).

<template>
  <div style="background-color: #212121;">
    <v-toolbar dense class="obspanel-toolbar">
      <v-spacer></v-spacer>
      <span class="px-2 grey--text">Analysis of {{ name }}</span>
      <v-spacer></v-spacer>
      <v-btn icon class="transparent" @click.stop="requestClose"><v-icon>mdi-close</v-icon></v-btn>
    </v-toolbar>
    <v-progress-circular v-if="inProgress" size=50 indeterminate style="position: absolute; margin-left: calc(50% - 25px); margin-top: calc(50vh - 25px); z-index: 1000;"></v-progress-circular>
    <div class="scroll-container" style="height: calc(100% - 48px); width: 100%">
      <v-container class="mr-0">
        <v-row>
          <v-col cols="5">
            <v-switch class="pt-0 pb-0 mt-0" dense v-model="cumulative" label="Cumulative"></v-switch>
            <!--<v-switch class="pt-0 pb-0 mt-0" dense v-model="showItemsWithoutDate" :disabled='!cumulative' :label="'Include items without ' + referenceField.name"></v-switch>-->
          </v-col>
          <v-col cols="7">
            <span>Time serie for field </span>
            <v-menu close-on-click>
              <template v-slot:activator="{ on, attrs }">
                <v-btn x-small text elevation="3" dark v-bind="attrs" v-on="on"> {{ referenceField.name }} <v-icon right>mdi-menu-down</v-icon></v-btn>
              </template>
              <v-list>
                <v-list-item v-for="(item, index) in $smt.fields.filter(f => f.type === 'date')" :key="index" @click="referenceField = item">
                  <v-list-item-title>{{ item.name }}</v-list-item-title>
                </v-list-item>
              </v-list>
            </v-menu>
          </v-col>
        </v-row>
        <smt-histogram class="mb-0" v-for="fr in results.fields" :key="fr.field.id" :data="fr" :cumulative="cumulative" :showItemsWithoutDate="showItemsWithoutDate">{{ fr.table }}</smt-histogram>
      </v-container>
    </div>

  </div>
</template>

<script>
import qe from '../query-engine'
import SmtHistogram from './smt-histogram.vue'
import Vue from 'vue'

export default {
  data: function () {
    return {
      cumulative: true,
      showItemsWithoutDate: true,
      inProgress: false,
      refreshCount: 0,
      referenceField: this.$smt.fields.find(f => f.id === 'CreationDate') || this.$smt.fields[0].id,
      results: {
        fields: []
      }
    }
  },
  props: ['name', 'constraints'],
  watch: {
    constraints: function () {
      this.refreshAllFields()
    },
    referenceField: function () {
      this.refreshAllFields()
    }
  },
  methods: {
    requestClose: function () {
      this.$emit('request-close')
    },
    refreshMinMax: function () {
      // Compute min/max for the field used to split the horizontal axis
      const q = {
        constraints: this.constraints,
        groupingOptions: [{ operation: 'GROUP_ALL' }],
        aggregationOptions: [{ operation: 'MIN_MAX', fieldId: this.referenceField.id, out: 'minmax' }]
      }
      return qe.query(q).then(res => {
        return res.res[0].minmax
      })
    },

    refreshAllFields: function () {
      const that = this
      that.inProgress = true
      that.results.fields.length = 0

      this.refreshCount++
      const refreshCount = this.refreshCount

      that.refreshMinMax().then(function (minmax) {
        if (refreshCount !== that.refreshCount) return
        if (!minmax || minmax[0] === minmax[1]) {
          that.inProgress = false
          return
        }

        const minSteps = 20
        const start = new Date(minmax[0])
        start.setUTCHours(0, 0, 0, 0)
        // Switch to next day and truncate
        const stop = new Date(minmax[1] + 1000 * 60 * 60 * 24)
        stop.setUTCHours(23, 59, 59, 0)
        // Range in days
        const range = (stop - start) / (1000 * 60 * 60 * 24)
        let step = 'day'
        if (range > minSteps * 365) {
          step = 'year'
        } else if (range > minSteps * 30) {
          step = 'month'
        }

        that.results.fields.push({
          field: { id: 'count', name: 'Items' },
          table: [['Date', 'Items']],
          loading: true
        })
        that.results.fields.push({
          field: { id: 'area', name: 'Area' },
          table: [['Date', 'Area']],
          loading: true
        })

        let q = {
          constraints: that.constraints,
          groupingOptions: [{ operation: 'GROUP_BY_DATE', fieldId: that.referenceField.id, step: step }],
          aggregationOptions: [
            { operation: 'COUNT', out: 'count' },
            { operation: 'MIN', fieldId: that.referenceField.id, out: 'x' }
          ]
        }
        for (const i in that.$smt.fields) {
          const field = that.$smt.fields[i]
          if (field.widget === 'tags') {
            q.aggregationOptions.push({ operation: 'VALUES_AND_COUNT', fieldId: field.id, out: field.id, limit: 8 })
          }
        }
        qe.query(q).then(res => {
          if (refreshCount !== that.refreshCount) return
          const lines = res.res
          for (const i in lines) {
            const d = new Date(lines[i].x)
            d.setUTCHours(0, 0, 0, 0)
            if (step === 'month') d.setUTCDate(1)
            if (step === 'year') {
              d.setUTCDate(1)
              d.setUTCMonth(0)
            }
            // Only keep the UTC date, skip the time
            if (lines[i].x !== null) {
              lines[i].x = new Date(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate())
            }
          }
          Vue.set(that.results.fields, 0, {
            field: { id: 'count', name: 'Items' },
            table: [['Date', 'Items']].concat(lines.map(l => [l.x, l.count])),
            step: step
          })
          for (const i in that.$smt.fields) {
            const field = that.$smt.fields[i]
            if (field.widget === 'tags') {
              let allValues = []
              const hasOverFlow = !!lines.find(l => Object.keys(l[field.id]).includes('__overflow'))
              if (hasOverFlow) continue
              lines.map(l => { allValues = allValues.concat(Object.keys(l[field.id])) })
              allValues = Array.from(new Set(allValues))
              const vals = lines.map(l => [l.x].concat(allValues.map(k => l[field.id][k])))
              that.results.fields.push({
                field: field,
                table: [['Date'].concat(allValues)].concat(vals),
                step: step
              })
            }
          }
          that.inProgress = false
        }, err => {
          console.log(err)
          that.inProgress = false
        })

        q = {
          constraints: that.constraints,
          groupingOptions: [{ operation: 'GROUP_ALL' }],
          aggregationOptions: [
            { operation: 'GEO_UNION_AREA_CUMULATED_DATE_HISTOGRAM', out: 'histo', dateFieldId: that.referenceField.id, step: step }
          ]
        }
        const strToDate = function (s) {
          if (s === null) return null
          const d = new Date(s)
          d.setUTCHours(0, 0, 0, 0)
          if (step === 'month') d.setUTCDate(1)
          if (step === 'year') {
            d.setUTCDate(1)
            d.setUTCMonth(0)
          }
          // Only keep the UTC date, skip the time
          return new Date(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate())
        }

        qe.query(q).then(res => {
          if (refreshCount !== that.refreshCount) return
          const lines = res.res[0].histo

          const STERADIAN_TO_DEG2 = (180 / Math.PI) * (180 / Math.PI)
          // De-cumulate the value, because the GUI re-cumulate them for display
          const arr = Object.keys(lines).map(k => [strToDate(k), lines[k] * STERADIAN_TO_DEG2])
          arr.sort((a, b) => {
            if (a[0] === b[0]) return 0
            if (a[0] === 'null') return -1
            if (b[0] === 'null') return 1
            return a[0] - b[0]
          })
          for (let i = arr.length - 1; i > 0; --i) {
            arr[i][1] -= arr[i - 1][1]
          }

          Vue.set(that.results.fields, 1, {
            field: { id: 'area', name: 'Area' },
            table: [['Date', 'Area (deg²)']].concat(arr),
            step: step
          })
        })
      })
    }
  },
  computed: {
  },
  created: function () {
  },
  mounted: function () {
    this.refreshAllFields()
  },
  components: { SmtHistogram }
}
</script>

<style>
.scroll-container {
  flex: 1 1 auto;
  overflow-y: scroll;
  backface-visibility: hidden;
}
</style>
